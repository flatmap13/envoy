#include "extensions/filters/http/health_check/health_check.h"

#include <chrono>
#include <string>

#include "envoy/event/dispatcher.h"
#include "envoy/event/timer.h"
#include "envoy/http/header_map.h"

#include "common/common/assert.h"
#include "common/common/enum_to_int.h"
#include "common/http/codes.h"
#include "common/http/header_map_impl.h"
#include "common/http/headers.h"
#include "common/http/utility.h"
#include "common/protobuf/utility.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace HealthCheck {

HealthCheckCacheManager::HealthCheckCacheManager(Event::Dispatcher& dispatcher,
                                                 std::chrono::milliseconds timeout)
    : clear_cache_timer_(dispatcher.createTimer([this]() -> void { onTimer(); })),
      timeout_(timeout) {
  onTimer();
}

void HealthCheckCacheManager::onTimer() {
  use_cached_response_ = false;
  clear_cache_timer_->enableTimer(timeout_);
}

Http::FilterHeadersStatus HealthCheckFilter::decodeHeaders(Http::HeaderMap& headers,
                                                           bool end_stream) {
  if (Http::HeaderUtility::matchHeaders(headers, *header_match_data_)) {
    health_check_request_ = true;
    callbacks_->streamInfo().healthCheck(true);

    // Set the 'sampled' status for the span to false. This overrides
    // any previous sampling decision associated with the trace instance,
    // resulting in this span (and any subsequent child spans) not being
    // reported to the backend tracing system.
    callbacks_->activeSpan().setSampled(false);

    // If we are not in pass through mode, we always handle. Otherwise, we handle if the server is
    // in the failed state or if we are using caching and we should use the cached response.
    if (!pass_through_mode_ || context_.healthCheckFailed() ||
        (cache_manager_ && cache_manager_->useCachedResponse())) {
      handling_ = true;
    }
  }

  if (end_stream && handling_) {
    onComplete();
  }

  return handling_ ? Http::FilterHeadersStatus::StopIteration : Http::FilterHeadersStatus::Continue;
}

Http::FilterDataStatus HealthCheckFilter::decodeData(Buffer::Instance&, bool end_stream) {
  if (end_stream && handling_) {
    onComplete();
  }

  return handling_ ? Http::FilterDataStatus::StopIterationNoBuffer
                   : Http::FilterDataStatus::Continue;
}

Http::FilterTrailersStatus HealthCheckFilter::decodeTrailers(Http::HeaderMap&) {
  if (handling_) {
    onComplete();
  }

  return handling_ ? Http::FilterTrailersStatus::StopIteration
                   : Http::FilterTrailersStatus::Continue;
}

Http::FilterHeadersStatus HealthCheckFilter::encodeHeaders(Http::HeaderMap& headers, bool) {
  if (health_check_request_) {
    if (cache_manager_) {
      cache_manager_->setCachedResponse(
          static_cast<Http::Code>(Http::Utility::getResponseStatus(headers)),
          headers.EnvoyDegraded() != nullptr);
    }

    headers.insertEnvoyUpstreamHealthCheckedCluster().value(context_.localInfo().clusterName());
  } else if (context_.healthCheckFailed()) {
    headers.insertEnvoyImmediateHealthCheckFail().value(
        Http::Headers::get().EnvoyImmediateHealthCheckFailValues.True);
  }

  return Http::FilterHeadersStatus::Continue;
}

void HealthCheckFilter::onComplete() {
  ASSERT(handling_);
  Http::Code final_status = Http::Code::OK;
  bool degraded = false;
  if (context_.healthCheckFailed()) {
    callbacks_->streamInfo().setResponseFlag(StreamInfo::ResponseFlag::FailedLocalHealthCheck);
    final_status = Http::Code::ServiceUnavailable;
  } else {
    if (cache_manager_) {
      const auto status_and_degraded = cache_manager_->getCachedResponse();
      final_status = status_and_degraded.first;
      degraded = status_and_degraded.second;
    } else if (cluster_min_healthy_percentages_ != nullptr &&
               !cluster_min_healthy_percentages_->empty()) {
      // Check the status of the specified upstream cluster(s) to determine the right response.
      auto& clusterManager = context_.clusterManager();
      for (const auto& item : *cluster_min_healthy_percentages_) {
        const std::string& cluster_name = item.first;
        const double min_healthy_percentage = item.second;
        auto* cluster = clusterManager.get(cluster_name);
        if (cluster == nullptr) {
          // If the cluster does not exist at all, consider the service unhealthy.
          final_status = Http::Code::ServiceUnavailable;
          break;
        }
        const auto& stats = cluster->info()->stats();
        const uint64_t membership_total = stats.membership_total_.value();
        if (membership_total == 0) {
          // If the cluster exists but is empty, consider the service unhealthy unless
          // the specified minimum percent healthy for the cluster happens to be zero.
          if (min_healthy_percentage == 0.0) {
            continue;
          } else {
            final_status = Http::Code::ServiceUnavailable;
            break;
          }
        }
        // In the general case, consider the service unhealthy if fewer than the
        // specified percentage of the servers in the cluster are available (healthy + degraded).
        // TODO(brian-pane) switch to purely integer-based math here, because the
        //                  int-to-float conversions and floating point division are slow.
        if ((stats.membership_healthy_.value() + stats.membership_degraded_.value()) <
            membership_total * min_healthy_percentage / 100.0) {
          final_status = Http::Code::ServiceUnavailable;
          break;
        }
      }
    }

    if (!Http::CodeUtility::is2xx(enumToInt(final_status))) {
      callbacks_->streamInfo().setResponseFlag(StreamInfo::ResponseFlag::FailedLocalHealthCheck);
    }
  }

  callbacks_->sendLocalReply(final_status, "",
                             [degraded](auto& headers) {
                               if (degraded) {
                                 headers.insertEnvoyDegraded();
                               }
                             },
                             absl::nullopt);
}

} // namespace HealthCheck
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
