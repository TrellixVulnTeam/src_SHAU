// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/dns/host_resolver_mojo.h"

#include "net/base/address_list.h"
#include "net/base/net_errors.h"
#include "net/dns/mojo_host_type_converters.h"
#include "net/log/net_log.h"
#include "third_party/mojo/src/mojo/public/cpp/bindings/binding.h"

namespace net {
namespace {

// Default TTL for successful host resolutions.
const int kCacheEntryTTLSeconds = 5;

// Default TTL for unsuccessful host resolutions.
const int kNegativeCacheEntryTTLSeconds = 0;

HostCache::Key CacheKeyForRequest(const HostResolver::RequestInfo& info) {
  return HostCache::Key(info.hostname(), info.address_family(),
                        info.host_resolver_flags());
}

}  // namespace

class HostResolverMojo::Job : public interfaces::HostResolverRequestClient {
 public:
  Job(const HostCache::Key& key,
      AddressList* addresses,
      const CompletionCallback& callback,
      mojo::InterfaceRequest<interfaces::HostResolverRequestClient> request,
      base::WeakPtr<HostCache> host_cache);

 private:
  // interfaces::HostResolverRequestClient override.
  void ReportResult(int32_t error,
                    interfaces::AddressListPtr address_list) override;

  // Mojo error handler.
  void OnConnectionError();

  const HostCache::Key key_;
  AddressList* addresses_;
  CompletionCallback callback_;
  mojo::Binding<interfaces::HostResolverRequestClient> binding_;
  base::WeakPtr<HostCache> host_cache_;
};

HostResolverMojo::HostResolverMojo(interfaces::HostResolverPtr resolver,
                                   const base::Closure& disconnect_callback)
    : resolver_(resolver.Pass()),
      disconnect_callback_(disconnect_callback),
      host_cache_(HostCache::CreateDefaultCache()),
      host_cache_weak_factory_(host_cache_.get()) {
  if (!disconnect_callback_.is_null()) {
    resolver_.set_connection_error_handler(base::Bind(
        &HostResolverMojo::OnConnectionError, base::Unretained(this)));
  }
}

HostResolverMojo::~HostResolverMojo() = default;

int HostResolverMojo::Resolve(const RequestInfo& info,
                              RequestPriority priority,
                              AddressList* addresses,
                              const CompletionCallback& callback,
                              RequestHandle* request_handle,
                              const BoundNetLog& source_net_log) {
  DCHECK(thread_checker_.CalledOnValidThread());
  DVLOG(1) << "Resolve " << info.host_port_pair().ToString();

  HostCache::Key key = CacheKeyForRequest(info);
  int cached_result = ResolveFromCacheInternal(info, key, addresses);
  if (cached_result != ERR_DNS_CACHE_MISS) {
    DVLOG(1) << "Resolved " << info.host_port_pair().ToString()
             << " from cache";
    return cached_result;
  }

  interfaces::HostResolverRequestClientPtr handle;
  *request_handle = new Job(key, addresses, callback, mojo::GetProxy(&handle),
                            host_cache_weak_factory_.GetWeakPtr());
  resolver_->Resolve(interfaces::HostResolverRequestInfo::From(info),
                     handle.Pass());
  return ERR_IO_PENDING;
}

int HostResolverMojo::ResolveFromCache(const RequestInfo& info,
                                       AddressList* addresses,
                                       const BoundNetLog& source_net_log) {
  DCHECK(thread_checker_.CalledOnValidThread());
  DVLOG(1) << "ResolveFromCache " << info.host_port_pair().ToString();
  return ResolveFromCacheInternal(info, CacheKeyForRequest(info), addresses);
}

void HostResolverMojo::CancelRequest(RequestHandle req) {
  DCHECK(thread_checker_.CalledOnValidThread());
  // Deleting the Job closes the HostResolverRequestClient connection,
  // signalling cancellation of the request.
  delete static_cast<Job*>(req);
}

HostCache* HostResolverMojo::GetHostCache() {
  return host_cache_.get();
}

void HostResolverMojo::OnConnectionError() {
  DCHECK(!disconnect_callback_.is_null());
  disconnect_callback_.Run();
}

int HostResolverMojo::ResolveFromCacheInternal(const RequestInfo& info,
                                               const HostCache::Key& key,
                                               AddressList* addresses) {
  if (!info.allow_cached_response())
    return ERR_DNS_CACHE_MISS;

  const HostCache::Entry* entry =
      host_cache_->Lookup(key, base::TimeTicks::Now());
  if (!entry)
    return ERR_DNS_CACHE_MISS;

  *addresses = AddressList::CopyWithPort(entry->addrlist, info.port());
  return entry->error;
}

HostResolverMojo::Job::Job(
    const HostCache::Key& key,
    AddressList* addresses,
    const CompletionCallback& callback,
    mojo::InterfaceRequest<interfaces::HostResolverRequestClient> request,
    base::WeakPtr<HostCache> host_cache)
    : key_(key),
      addresses_(addresses),
      callback_(callback),
      binding_(this, request.Pass()),
      host_cache_(host_cache) {
  binding_.set_connection_error_handler(base::Bind(
      &HostResolverMojo::Job::OnConnectionError, base::Unretained(this)));
}

void HostResolverMojo::Job::ReportResult(
    int32_t error,
    interfaces::AddressListPtr address_list) {
  if (error == OK && address_list)
    *addresses_ = address_list->To<AddressList>();
  if (host_cache_) {
    base::TimeDelta ttl = base::TimeDelta::FromSeconds(
        error == OK ? kCacheEntryTTLSeconds : kNegativeCacheEntryTTLSeconds);
    HostCache::Entry entry(error, *addresses_, ttl);
    host_cache_->Set(key_, entry, base::TimeTicks::Now(), ttl);
  }
  callback_.Run(error);
  delete this;
}

void HostResolverMojo::Job::OnConnectionError() {
  ReportResult(ERR_FAILED, interfaces::AddressListPtr());
}

}  // namespace net
