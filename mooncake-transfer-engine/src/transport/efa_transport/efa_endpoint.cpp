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

#include "transport/efa_transport/efa_endpoint.h"

#include <glog/logging.h>

#include <cassert>
#include <cstddef>
#include <cstring>
#include <iomanip>
#include <sstream>

#include "config.h"

namespace mooncake {

EfaEndPoint::EfaEndPoint(EfaContext &context)
    : context_(context),
      status_(INITIALIZING),
      ep_(nullptr),
      tx_cq_(nullptr),
      rx_cq_(nullptr),
      peer_fi_addr_(FI_ADDR_UNSPEC),
      local_addr_len_(0),
      wr_depth_(0),
      max_wr_depth_(0),
      cq_outstanding_(nullptr),
      active_(true),
      inactive_time_(0) {}

EfaEndPoint::~EfaEndPoint() {
    if (ep_) deconstruct();
}

int EfaEndPoint::construct(struct fid_cq *cq, size_t num_qp_list,
                           size_t max_sge, size_t max_wr,
                           size_t max_inline) {
    if (status_.load(std::memory_order_relaxed) != INITIALIZING) {
        LOG(ERROR) << "EFA Endpoint has already been constructed";
        return ERR_ENDPOINT;
    }

    tx_cq_ = cq;
    rx_cq_ = cq;  // Use same CQ for TX and RX
    max_wr_depth_ = max_wr;

    // Create endpoint
    int ret = fi_endpoint(context_.domain(), context_.info(), &ep_, nullptr);
    if (ret) {
        LOG(ERROR) << "fi_endpoint failed: " << fi_strerror(-ret);
        return ERR_ENDPOINT;
    }

    // Bind endpoint to AV
    ret = fi_ep_bind(ep_, &context_.av()->fid, 0);
    if (ret) {
        LOG(ERROR) << "fi_ep_bind (av) failed: " << fi_strerror(-ret);
        fi_close(&ep_->fid);
        ep_ = nullptr;
        return ERR_ENDPOINT;
    }

    // Bind endpoint to TX CQ
    ret = fi_ep_bind(ep_, &tx_cq_->fid, FI_TRANSMIT);
    if (ret) {
        LOG(ERROR) << "fi_ep_bind (tx_cq) failed: " << fi_strerror(-ret);
        fi_close(&ep_->fid);
        ep_ = nullptr;
        return ERR_ENDPOINT;
    }

    // Bind endpoint to RX CQ
    ret = fi_ep_bind(ep_, &rx_cq_->fid, FI_RECV);
    if (ret) {
        LOG(ERROR) << "fi_ep_bind (rx_cq) failed: " << fi_strerror(-ret);
        fi_close(&ep_->fid);
        ep_ = nullptr;
        return ERR_ENDPOINT;
    }

    // Enable endpoint
    ret = fi_enable(ep_);
    if (ret) {
        LOG(ERROR) << "fi_enable failed: " << fi_strerror(-ret);
        fi_close(&ep_->fid);
        ep_ = nullptr;
        return ERR_ENDPOINT;
    }

    // Get local endpoint address
    local_addr_len_ = 64;  // EFA addresses are typically 32 bytes
    local_addr_.resize(local_addr_len_);
    ret = fi_getname(&ep_->fid, local_addr_.data(), &local_addr_len_);
    if (ret) {
        LOG(ERROR) << "fi_getname failed: " << fi_strerror(-ret);
        fi_close(&ep_->fid);
        ep_ = nullptr;
        return ERR_ENDPOINT;
    }
    local_addr_.resize(local_addr_len_);

    status_.store(UNCONNECTED, std::memory_order_relaxed);
    return 0;
}

int EfaEndPoint::deconstruct() {
    if (ep_) {
        fi_close(&ep_->fid);
        ep_ = nullptr;
    }
    return 0;
}

int EfaEndPoint::destroyQP() {
    return deconstruct();
}

void EfaEndPoint::setPeerNicPath(const std::string &peer_nic_path) {
    RWSpinlock::WriteGuard guard(lock_);
    if (connected()) {
        LOG(WARNING) << "Previous EFA connection will be discarded";
        disconnectUnlocked();
    }
    peer_nic_path_ = peer_nic_path;
}

std::string EfaEndPoint::getLocalAddr() const {
    std::ostringstream oss;
    for (size_t i = 0; i < local_addr_.size(); ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)local_addr_[i];
    }
    return oss.str();
}

int EfaEndPoint::insertPeerAddr(const std::string &peer_addr) {
    // Convert hex string to binary address
    std::vector<uint8_t> addr_bin;
    addr_bin.reserve(peer_addr.size() / 2);

    for (size_t i = 0; i < peer_addr.size(); i += 2) {
        std::string byte_str = peer_addr.substr(i, 2);
        uint8_t byte = (uint8_t)strtol(byte_str.c_str(), nullptr, 16);
        addr_bin.push_back(byte);
    }

    // Insert into address vector
    int ret = fi_av_insert(context_.av(), addr_bin.data(), 1, &peer_fi_addr_, 0, nullptr);
    if (ret != 1) {
        LOG(ERROR) << "fi_av_insert failed: " << fi_strerror(-ret);
        return ERR_ENDPOINT;
    }

    return 0;
}

int EfaEndPoint::setupConnectionsByActive() {
    RWSpinlock::WriteGuard guard(lock_);
    if (connected()) {
        LOG(INFO) << "EFA Connection has been established";
        return 0;
    }

    // Loopback mode
    if (context_.nicPath() == peer_nic_path_) {
        // For loopback, insert our own address
        int ret = insertPeerAddr(getLocalAddr());
        if (ret != 0) {
            return ret;
        }
        status_.store(CONNECTED, std::memory_order_release);
        LOG(INFO) << "EFA loopback connection established: " << toString();
        return 0;
    }

    // Exchange addresses via handshake
    TransferMetadata::HandShakeDesc local_desc, peer_desc;
    local_desc.local_nic_path = context_.nicPath();
    local_desc.peer_nic_path = peer_nic_path_;
    // Store our endpoint address in the handshake (using qp_num field for compatibility)
    // We'll encode the address as a hex string in reply_msg for now
    local_desc.reply_msg = getLocalAddr();

    auto peer_server_name = getServerNameFromNicPath(peer_nic_path_);
    auto peer_nic_name = getNicNameFromNicPath(peer_nic_path_);
    if (peer_server_name.empty() || peer_nic_name.empty()) {
        LOG(ERROR) << "Parse peer EFA nic path failed: " << peer_nic_path_;
        return ERR_INVALID_ARGUMENT;
    }

    int rc = context_.engine().sendHandshake(peer_server_name, local_desc, peer_desc);
    if (rc) return rc;

    if (peer_desc.reply_msg.empty()) {
        LOG(ERROR) << "Peer did not provide EFA address in handshake";
        return ERR_REJECT_HANDSHAKE;
    }

    // Insert peer's address into our AV
    rc = insertPeerAddr(peer_desc.reply_msg);
    if (rc != 0) {
        return rc;
    }

    status_.store(CONNECTED, std::memory_order_release);
    LOG(INFO) << "EFA connection established: " << toString();
    return 0;
}

int EfaEndPoint::setupConnectionsByPassive(const HandShakeDesc &peer_desc,
                                           HandShakeDesc &local_desc) {
    RWSpinlock::WriteGuard guard(lock_);
    if (connected()) {
        LOG(WARNING) << "Re-establish EFA connection: " << toString();
        disconnectUnlocked();
    }

    if (peer_desc.peer_nic_path != context_.nicPath() ||
        peer_desc.local_nic_path != peer_nic_path_) {
        local_desc.reply_msg = "";
        LOG(ERROR) << "Invalid argument: peer EFA nic path inconsistency";
        return ERR_REJECT_HANDSHAKE;
    }

    // Insert peer's address from handshake
    if (peer_desc.reply_msg.empty()) {
        local_desc.reply_msg = "";
        LOG(ERROR) << "Peer did not provide EFA address";
        return ERR_REJECT_HANDSHAKE;
    }

    int ret = insertPeerAddr(peer_desc.reply_msg);
    if (ret != 0) {
        local_desc.reply_msg = "";
        return ret;
    }

    // Provide our address to peer
    local_desc.local_nic_path = context_.nicPath();
    local_desc.peer_nic_path = peer_nic_path_;
    local_desc.reply_msg = getLocalAddr();

    status_.store(CONNECTED, std::memory_order_release);
    LOG(INFO) << "EFA connection established (passive): " << toString();
    return 0;
}

void EfaEndPoint::disconnect() {
    RWSpinlock::WriteGuard guard(lock_);
    disconnectUnlocked();
}

void EfaEndPoint::disconnectUnlocked() {
    // For EFA RDM endpoints, we don't need to reset QP state
    // Just remove peer from AV if needed and mark as disconnected
    peer_fi_addr_ = FI_ADDR_UNSPEC;
    status_.store(UNCONNECTED, std::memory_order_release);
}

const std::string EfaEndPoint::toString() const {
    return "EfaEndPoint[" + context_.nicPath() + " <-> " + peer_nic_path_ + "]";
}

bool EfaEndPoint::hasOutstandingSlice() const {
    return wr_depth_ > 0;
}

int EfaEndPoint::doSetupConnection(const std::string &peer_addr,
                                   std::string *reply_msg) {
    int ret = insertPeerAddr(peer_addr);
    if (ret != 0) {
        if (reply_msg) *reply_msg = "Failed to insert peer address into AV";
        return ret;
    }

    status_.store(CONNECTED, std::memory_order_release);
    return 0;
}

int EfaEndPoint::submitPostSend(std::vector<Transport::Slice *> &slice_list,
                                std::vector<Transport::Slice *> &failed_slice_list) {
    if (!connected()) {
        // Try to establish connection first
        int ret = setupConnectionsByActive();
        if (ret != 0) {
            // Move all slices to failed list
            for (auto *slice : slice_list) {
                failed_slice_list.push_back(slice);
            }
            slice_list.clear();
            return ret;
        }
    }

    // Process slices - for now using fi_write for RDMA write operations
    for (auto it = slice_list.begin(); it != slice_list.end();) {
        Transport::Slice *slice = *it;

        // Get memory region descriptor for the local buffer
        void *local_desc = nullptr;
        // In a full implementation, we'd look up the MR descriptor

        // Post RDMA write using the rdma union member
        struct fi_context ctx;
        memset(&ctx, 0, sizeof(ctx));

        ssize_t ret = fi_write(ep_,
                               (void*)slice->source_addr,  // local buffer
                               slice->length,
                               local_desc,
                               peer_fi_addr_,
                               slice->rdma.dest_addr,  // remote address
                               slice->rdma.dest_rkey,  // remote key
                               &ctx);

        if (ret == 0) {
            // Successfully posted
            wr_depth_++;
            slice->markSuccess();
            it = slice_list.erase(it);
        } else if (ret == -FI_EAGAIN) {
            // Queue full, try again later
            ++it;
        } else {
            // Error
            LOG(ERROR) << "fi_write failed: " << fi_strerror(-ret);
            failed_slice_list.push_back(slice);
            it = slice_list.erase(it);
        }
    }

    return 0;
}

}  // namespace mooncake
