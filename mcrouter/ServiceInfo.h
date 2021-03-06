/*
 *  Copyright (c) 2016, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include <memory>

#include <folly/Range.h>

#include "mcrouter/lib/McRequest.h"

namespace facebook { namespace memcache { namespace mcrouter {

class ProxyConfig;
template <class Request>
class ProxyRequestContextTyped;
struct proxy_t;

/**
 * Answers mc_op_get_service_info requests of the form
 * __mcrouter__.commands(args,...)
 */
class ServiceInfo {
 public:
  using ContextType = ProxyRequestContextTyped<McRequestWithMcOp<mc_op_get>>;
  ServiceInfo(proxy_t* proxy, const ProxyConfig& config);

  void handleRequest(folly::StringPiece req,
                     const std::shared_ptr<ContextType>& ctx) const;
  ~ServiceInfo();

 private:
  class ServiceInfoImpl;
  std::unique_ptr<ServiceInfoImpl> impl_;
};

}}}  // facebook::memcache::mcrouter
