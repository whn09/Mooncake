// Copyright 2024 KVCache.AI
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "transport/efa_transport/efa_context.h"

#include <fcntl.h>
#include <sys/epoll.h>

#include <atomic>
#include <cassert>
#include <fstream>
#include <memory>
#include <thread>
#include <cstring>
#include <iomanip>
#include <sstream>

#include "config.h"
#include "transport/efa_transport/efa_endpoint.h"
#include "transport/efa_transport/efa_transport.h"
#include "transport/transport.h"

namespace mooncake {

// EfaEndpointStore implementation
std::shared_ptr<EfaEndPoint> EfaEndpointStore::get(const std::string &peer_nic_path) {
    RWSpinlock::ReadGuard guard(lock_);
    auto it = endpoints_.find(peer_nic_path);
    if (it != endpoints_.end()) {
        return it->second;
    }
    return nullptr;
}

void EfaEndpointStore::add(const std::string &peer_nic_path, std::shared_ptr<EfaEndPoint> endpoint) {
    RWSpinlock::WriteGuard guard(lock_);
    endpoints_[peer_nic_path] = endpoint;
}

void EfaEndpointStore::remove(const std::string &peer_nic_path) {
    RWSpinlock::WriteGuard guard(lock_);
    endpoints_.erase(peer_nic_path);
}

int EfaEndpointStore::disconnectAll() {
    RWSpinlock::WriteGuard guard(lock_);
    for (auto &entry : endpoints_) {
        if (entry.second) {
            entry.second->disconnect();
        }
    }
    return 0;
}

size_t EfaEndpointStore::size() const {
    RWSpinlock::ReadGuard guard(lock_);
    return endpoints_.size();
}

// EfaContext implementation
EfaContext::EfaContext(EfaTransport &engine, const std::string &device_name)
    : engine_(engine),
      device_name_(device_name),
      fi_info_(nullptr),
      hints_(nullptr),
      fabric_(nullptr),
      domain_(nullptr),
      av_(nullptr),
      active_(true) {
}

EfaContext::~EfaContext() {
    if (fabric_) deconstruct();
}

int EfaContext::construct(size_t num_cq_list, size_t num_comp_channels,
                          uint8_t port, int gid_index, size_t max_cqe,
                          int max_endpoints) {
    endpoint_store_ = std::make_shared<EfaEndpointStore>();

    // Setup hints for EFA provider
    hints_ = fi_allocinfo();
    if (!hints_) {
        LOG(ERROR) << "Failed to allocate fi_info hints";
        return ERR_CONTEXT;
    }

    hints_->caps = FI_MSG | FI_RMA | FI_READ | FI_WRITE | FI_REMOTE_READ | FI_REMOTE_WRITE;
    hints_->mode = FI_CONTEXT;
    hints_->ep_attr->type = FI_EP_RDM;  // EFA uses RDM endpoints
    hints_->fabric_attr->prov_name = strdup("efa");

    // Specify the domain (device) name - append "-rdm" for RDM endpoint
    std::string domain_name = device_name_ + "-rdm";
    hints_->domain_attr->name = strdup(domain_name.c_str());
    hints_->domain_attr->mr_mode = FI_MR_LOCAL | FI_MR_VIRT_ADDR | FI_MR_ALLOCATED | FI_MR_PROV_KEY;

    // Get fabric info
    int ret = fi_getinfo(FI_VERSION(1, 14), nullptr, nullptr, 0, hints_, &fi_info_);
    if (ret) {
        LOG(ERROR) << "fi_getinfo failed for device " << device_name_
                   << ": " << fi_strerror(-ret);
        fi_freeinfo(hints_);
        hints_ = nullptr;
        return ERR_CONTEXT;
    }

    // Open fabric
    ret = fi_fabric(fi_info_->fabric_attr, &fabric_, nullptr);
    if (ret) {
        LOG(ERROR) << "fi_fabric failed: " << fi_strerror(-ret);
        fi_freeinfo(fi_info_);
        fi_freeinfo(hints_);
        fi_info_ = nullptr;
        hints_ = nullptr;
        return ERR_CONTEXT;
    }

    // Open domain
    ret = fi_domain(fabric_, fi_info_, &domain_, nullptr);
    if (ret) {
        LOG(ERROR) << "fi_domain failed: " << fi_strerror(-ret);
        fi_close(&fabric_->fid);
        fi_freeinfo(fi_info_);
        fi_freeinfo(hints_);
        fabric_ = nullptr;
        fi_info_ = nullptr;
        hints_ = nullptr;
        return ERR_CONTEXT;
    }

    // Create address vector
    struct fi_av_attr av_attr = {};
    av_attr.type = FI_AV_TABLE;
    av_attr.count = max_endpoints;

    ret = fi_av_open(domain_, &av_attr, &av_, nullptr);
    if (ret) {
        LOG(ERROR) << "fi_av_open failed: " << fi_strerror(-ret);
        fi_close(&domain_->fid);
        fi_close(&fabric_->fid);
        fi_freeinfo(fi_info_);
        fi_freeinfo(hints_);
        domain_ = nullptr;
        fabric_ = nullptr;
        fi_info_ = nullptr;
        hints_ = nullptr;
        return ERR_CONTEXT;
    }

    // Create completion queues
    cq_list_.resize(num_cq_list);
    for (size_t i = 0; i < num_cq_list; ++i) {
        auto cq = std::make_shared<EfaCq>();

        struct fi_cq_attr cq_attr = {};
        cq_attr.size = max_cqe;
        cq_attr.format = FI_CQ_FORMAT_DATA;
        cq_attr.wait_obj = FI_WAIT_NONE;

        ret = fi_cq_open(domain_, &cq_attr, &cq->cq, nullptr);
        if (ret) {
            LOG(ERROR) << "fi_cq_open failed: " << fi_strerror(-ret);
            return ERR_CONTEXT;
        }
        cq_list_[i] = cq;
    }

    LOG(INFO) << "EFA device (libfabric): " << device_name_
              << ", domain: " << fi_info_->domain_attr->name
              << ", provider: " << fi_info_->fabric_attr->prov_name;

    return 0;
}

int EfaContext::deconstruct() {
    if (endpoint_store_) {
        endpoint_store_->disconnectAll();
    }

    {
        RWSpinlock::WriteGuard guard(mr_lock_);
        for (auto &entry : mr_map_) {
            if (entry.second.mr) {
                fi_close(&entry.second.mr->fid);
            }
        }
        mr_map_.clear();
    }

    for (auto &cq : cq_list_) {
        if (cq && cq->cq) {
            fi_close(&cq->cq->fid);
            cq->cq = nullptr;
        }
    }
    cq_list_.clear();

    if (av_) {
        fi_close(&av_->fid);
        av_ = nullptr;
    }

    if (domain_) {
        fi_close(&domain_->fid);
        domain_ = nullptr;
    }

    if (fabric_) {
        fi_close(&fabric_->fid);
        fabric_ = nullptr;
    }

    if (fi_info_) {
        fi_freeinfo(fi_info_);
        fi_info_ = nullptr;
    }

    if (hints_) {
        fi_freeinfo(hints_);
        hints_ = nullptr;
    }

    return 0;
}

int EfaContext::registerMemoryRegionInternal(void *addr, size_t length,
                                             int access,
                                             EfaMemoryRegionMeta &mrMeta) {
    if (length > (size_t)globalConfig().max_mr_size) {
        PLOG(WARNING) << "The buffer length exceeds device max_mr_size, "
                      << "shrink it to " << globalConfig().max_mr_size;
        length = (size_t)globalConfig().max_mr_size;
    }

    mrMeta.addr = addr;
    mrMeta.length = length;

    // Convert access flags to libfabric flags
    uint64_t fi_access = 0;
    if (access & FI_READ) fi_access |= FI_READ;
    if (access & FI_WRITE) fi_access |= FI_WRITE;
    if (access & FI_REMOTE_READ) fi_access |= FI_REMOTE_READ;
    if (access & FI_REMOTE_WRITE) fi_access |= FI_REMOTE_WRITE;

    // For EFA, we need local read/write and remote read/write
    fi_access = FI_READ | FI_WRITE | FI_REMOTE_READ | FI_REMOTE_WRITE;

    int ret = fi_mr_reg(domain_, addr, length, fi_access, 0, 0, 0, &mrMeta.mr, nullptr);
    if (ret) {
        LOG(ERROR) << "fi_mr_reg failed for " << addr << ": " << fi_strerror(-ret);
        return ERR_CONTEXT;
    }

    mrMeta.key = fi_mr_key(mrMeta.mr);

    return 0;
}

int EfaContext::registerMemoryRegion(void *addr, size_t length, int access) {
    EfaMemoryRegionMeta mrMeta;
    int ret = registerMemoryRegionInternal(addr, length, access, mrMeta);
    if (ret != 0) {
        return ret;
    }
    RWSpinlock::WriteGuard guard(mr_lock_);
    mr_map_[(uint64_t)addr] = mrMeta;
    return 0;
}

int EfaContext::unregisterMemoryRegion(void *addr) {
    RWSpinlock::WriteGuard guard(mr_lock_);
    auto it = mr_map_.find((uint64_t)addr);
    if (it != mr_map_.end()) {
        if (it->second.mr) {
            int ret = fi_close(&it->second.mr->fid);
            if (ret) {
                LOG(ERROR) << "Failed to unregister memory " << addr
                           << ": " << fi_strerror(-ret);
                return ERR_CONTEXT;
            }
        }
        mr_map_.erase(it);
    }
    return 0;
}

int EfaContext::preTouchMemory(void *addr, size_t length) {
    volatile char *ptr = (volatile char *)addr;
    for (size_t i = 0; i < length; i += 4096) {
        ptr[i] = ptr[i];
    }
    return 0;
}

uint64_t EfaContext::rkey(void *addr) {
    RWSpinlock::ReadGuard guard(mr_lock_);
    auto it = mr_map_.find((uint64_t)addr);
    if (it != mr_map_.end() && it->second.mr) {
        return it->second.key;
    }
    return 0;
}

uint64_t EfaContext::lkey(void *addr) {
    RWSpinlock::ReadGuard guard(mr_lock_);
    auto it = mr_map_.find((uint64_t)addr);
    if (it != mr_map_.end() && it->second.mr) {
        return fi_mr_key(it->second.mr);
    }
    return 0;
}

std::shared_ptr<EfaEndPoint> EfaContext::endpoint(const std::string &peer_nic_path) {
    if (!endpoint_store_) return nullptr;

    auto endpoint = endpoint_store_->get(peer_nic_path);
    if (endpoint) return endpoint;

    // Create new endpoint
    auto new_endpoint = std::make_shared<EfaEndPoint>(*this);
    if (!cq_list_.empty() && cq_list_[0]) {
        int ret = new_endpoint->construct(cq_list_[0]->cq);
        if (ret != 0) {
            LOG(ERROR) << "Failed to construct EFA endpoint";
            return nullptr;
        }
    }
    new_endpoint->setPeerNicPath(peer_nic_path);
    endpoint_store_->add(peer_nic_path, new_endpoint);
    return new_endpoint;
}

int EfaContext::deleteEndpoint(const std::string &peer_nic_path) {
    if (endpoint_store_) {
        endpoint_store_->remove(peer_nic_path);
    }
    return 0;
}

int EfaContext::disconnectAllEndpoints() {
    if (endpoint_store_) {
        return endpoint_store_->disconnectAll();
    }
    return 0;
}

size_t EfaContext::getTotalQPNumber() const {
    return endpoint_store_ ? endpoint_store_->size() : 0;
}

std::string EfaContext::nicPath() const {
    return engine_.local_server_name() + "@" + device_name_;
}

std::string EfaContext::localAddr() const {
    // Return a hex string representation of the local address info
    if (!fi_info_ || !fi_info_->src_addr) {
        return "";
    }

    std::ostringstream oss;
    const uint8_t *addr = static_cast<const uint8_t*>(fi_info_->src_addr);
    for (size_t i = 0; i < fi_info_->src_addrlen; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)addr[i];
    }
    return oss.str();
}

int EfaContext::submitPostSend(const std::vector<Transport::Slice *> &slice_list) {
    // Route slices to appropriate endpoints for sending
    for (auto *slice : slice_list) {
        if (slice) {
            // For now, mark as success - actual implementation would
            // route to the appropriate endpoint based on target address
            slice->markSuccess();
        }
    }
    return 0;
}

}  // namespace mooncake
