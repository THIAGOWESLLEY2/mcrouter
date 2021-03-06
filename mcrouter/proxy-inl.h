/*
 *  Copyright (c) 2016, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "mcrouter/McrouterFiberContext.h"
#include "mcrouter/ProxyRequestContext.h"
#include "mcrouter/routes/ProxyRoute.h"
#include "mcrouter/ServiceInfo.h"
#include "mcrouter/stats.h"

namespace facebook {
namespace memcache {
namespace mcrouter {

namespace detail {

bool processGetServiceInfoRequest(
    const McRequestWithMcOp<mc_op_get>& req,
    std::shared_ptr<ProxyRequestContextTyped<
        McRequestWithMcOp<mc_op_get>>>& ctx);

template <class Request>
bool processGetServiceInfoRequest(
    const Request&,
    std::shared_ptr<ProxyRequestContextTyped<Request>>&) {

  return false;
}

} // detail

template <class Request>
proxy_t::WaitingRequest<Request>::WaitingRequest(
    const Request& req,
    std::unique_ptr<ProxyRequestContextTyped<Request>> ctx)
    : req_(req), ctx_(std::move(ctx)) {}

template <class Request>
void proxy_t::WaitingRequest<Request>::process(proxy_t* proxy) {
  // timePushedOnQueue_ is nonnegative only if waiting-requests-timeout is
  // enabled
  if (timePushedOnQueue_ >= 0) {
    const auto durationInQueueUs = nowUs() - timePushedOnQueue_;

    if (durationInQueueUs >
        1000 * static_cast<int64_t>(
          proxy->getRouterOptions().waiting_request_timeout_ms)) {
      ctx_->sendReply(mc_res_timeout, "Waiting request timeout exceeded");
      return;
    }
  }

  proxy->processRequest(req_, std::move(ctx_));
}

template <class Request>
typename std::enable_if<RequestListContains<Request>::value ||
                        TRequestListContains<Request>::value,
                        void>::type
proxy_t::routeHandlesProcessRequest(
    const Request& req,
    std::unique_ptr<ProxyRequestContextTyped<Request>> uctx) {

  auto sharedCtx = ProxyRequestContextTyped<Request>::process(
      std::move(uctx), getConfig());

  if (detail::processGetServiceInfoRequest(req, sharedCtx)) {
    return;
  }

  auto funcCtx = sharedCtx;

  fiberManager.addTaskFinally(
      [&req, ctx = std::move(funcCtx)]() mutable {
        try {
          auto& proute = ctx->proxyRoute();
          fiber_local::setSharedCtx(std::move(ctx));
          return proute.route(req);
        } catch (const std::exception& e) {
          auto err = folly::sformat(
              "Error routing request of type {}!"
              " Exception: {}",
              typeid(Request).name(), e.what());
          return ReplyT<Request>(mc_res_local_error, std::move(err));
        }
      },
      [ctx = std::move(sharedCtx)](
          folly::Try<ReplyT<Request>>&& reply) {
        ctx->sendReply(std::move(*reply));
      });
}

template <class Request>
typename std::enable_if<!RequestListContains<Request>::value &&
                        !TRequestListContains<Request>::value,
                        void>::type
proxy_t::routeHandlesProcessRequest(
    const Request&,
    std::unique_ptr<ProxyRequestContextTyped<Request>> uctx) {

  auto err = folly::sformat(
      "Couldn't route request of type {} "
      "because the operation is not supported by RouteHandles "
      "library!",
      typeid(Request).name());
  uctx->sendReply(mc_res_local_error, err);
}

template <class Request>
void proxy_t::processRequest(
    const Request& req,
    std::unique_ptr<ProxyRequestContextTyped<Request>> ctx) {

  assert(!ctx->processing_);
  ctx->processing_ = true;
  ++numRequestsProcessing_;
  stat_incr(stats, proxy_reqs_processing_stat, 1);
  bumpStats(req);

  routeHandlesProcessRequest(req, std::move(ctx));

  stat_incr(stats, request_sent_stat, 1);
  stat_incr(stats, request_sent_count_stat, 1);
}

template <class Request>
void proxy_t::dispatchRequest(
    const Request& req,
    std::unique_ptr<ProxyRequestContextTyped<Request>> ctx) {

  if (rateLimited(ctx->priority(), req)) {
    if (getRouterOptions().proxy_max_throttled_requests > 0 &&
        numRequestsWaiting_ >=
            getRouterOptions().proxy_max_throttled_requests) {
      ctx->sendReply(mc_res_local_error, "Max throttled exceeded");
      return;
    }
    auto& queue = waitingRequests_[static_cast<int>(ctx->priority())];
    auto w = folly::make_unique<WaitingRequest<Request>>(
        req, std::move(ctx));
    // Only enable timeout on waitingRequests_ queue when queue throttling is
    // enabled
    if (getRouterOptions().proxy_max_inflight_requests > 0 &&
        getRouterOptions().proxy_max_throttled_requests > 0 &&
        getRouterOptions().waiting_request_timeout_ms > 0) {
      w->setTimePushedOnQueue(nowUs());
    }
    queue.pushBack(std::move(w));
    ++numRequestsWaiting_;
    stat_incr(stats, proxy_reqs_waiting_stat, 1);
  } else {
    processRequest(req, std::move(ctx));
  }
}

template <>
inline void proxy_t::bumpStats(
    const McRequestWithMcOp<mc_op_stats>&) {
  stat_incr(stats, cmd_stats_stat, 1);
  stat_incr(stats, cmd_stats_count_stat, 1);
}

template <>
inline void proxy_t::bumpStats(
    const McRequestWithMcOp<mc_op_cas>&) {
  stat_incr(stats, cmd_cas_stat, 1);
  stat_incr(stats, cmd_cas_count_stat, 1);
}

template <>
inline void proxy_t::bumpStats(
    const McRequestWithMcOp<mc_op_get>&) {
  stat_incr(stats, cmd_get_stat, 1);
  stat_incr(stats, cmd_get_count_stat, 1);
}

template <>
inline void proxy_t::bumpStats(
    const McRequestWithMcOp<mc_op_gets>&) {
  stat_incr(stats, cmd_gets_stat, 1);
  stat_incr(stats, cmd_gets_count_stat, 1);
}

template <>
inline void proxy_t::bumpStats(
    const McRequestWithMcOp<mc_op_metaget>&) {
  stat_incr(stats, cmd_meta_stat, 1);
}

template <>
inline void proxy_t::bumpStats(
    const McRequestWithMcOp<mc_op_add>&) {
  stat_incr(stats, cmd_add_stat, 1);
  stat_incr(stats, cmd_add_count_stat, 1);
}

template <>
inline void proxy_t::bumpStats(
    const McRequestWithMcOp<mc_op_replace>&) {
  stat_incr(stats, cmd_replace_stat, 1);
  stat_incr(stats, cmd_replace_count_stat, 1);
}

template <>
inline void proxy_t::bumpStats(
    const McRequestWithMcOp<mc_op_set>&) {
  stat_incr(stats, cmd_set_stat, 1);
  stat_incr(stats, cmd_set_count_stat, 1);
}

template <>
inline void proxy_t::bumpStats(
    const McRequestWithMcOp<mc_op_incr>&) {
  stat_incr(stats, cmd_incr_stat, 1);
  stat_incr(stats, cmd_incr_count_stat, 1);
}

template <>
inline void proxy_t::bumpStats(
    const McRequestWithMcOp<mc_op_decr>&) {
  stat_incr(stats, cmd_decr_stat, 1);
  stat_incr(stats, cmd_decr_count_stat, 1);
}

template <>
inline void proxy_t::bumpStats(
    const McRequestWithMcOp<mc_op_delete>&) {
  stat_incr(stats, cmd_delete_stat, 1);
  stat_incr(stats, cmd_delete_count_stat, 1);
}

template <>
inline void proxy_t::bumpStats(
    const McRequestWithMcOp<mc_op_lease_set>&) {
  stat_incr(stats, cmd_lease_set_stat, 1);
  stat_incr(stats, cmd_lease_set_count_stat, 1);
}

template <>
inline void proxy_t::bumpStats(
    const McRequestWithMcOp<mc_op_lease_get>&) {
  stat_incr(stats, cmd_lease_get_stat, 1);
  stat_incr(stats, cmd_lease_get_count_stat, 1);
}

template <class Request>
inline void proxy_t::bumpStats(const Request&) {
  stat_incr(stats, cmd_other_stat, 1);
  stat_incr(stats, cmd_other_count_stat, 1);
}

template <>
inline bool proxy_t::rateLimited(
    ProxyRequestPriority priority,
    const McRequestWithMcOp<mc_op_stats>&) const {
  return false;
}

template <>
inline bool proxy_t::rateLimited(
    ProxyRequestPriority priority,
    const McRequestWithMcOp<mc_op_version>&) const {
  return false;
}

template <>
inline bool proxy_t::rateLimited(
    ProxyRequestPriority priority,
    const McRequestWithMcOp<mc_op_get_service_info>&) const {
  return false;
}

template <class Request>
inline bool proxy_t::rateLimited(ProxyRequestPriority priority,
                                 const Request&) const {
  if (!getRouterOptions().proxy_max_inflight_requests) {
    return false;
  }

  if (waitingRequests_[static_cast<int>(priority)].empty() &&
      numRequestsProcessing_ < getRouterOptions().proxy_max_inflight_requests) {
    return false;
  }

  return true;
}
} // mcrouter
} // memcache
} // facebook
