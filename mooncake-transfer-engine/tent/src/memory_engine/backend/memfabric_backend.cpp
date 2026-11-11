// Copyright 2026 KVCache.AI
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

#include "tent/memory_engine/backend/memfabric_backend.h"

#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

#include <glog/logging.h>

namespace mooncake {
namespace tent {
namespace {

int32_t ParseDataOpType() {
    const char* value = std::getenv("MC_MEMORY_ENGINE_DATA_OP");
    const std::string name = (value && *value) ? value : "sdma";
    if (name == "sdma") return smem_bm::kDataOpSdma;
    if (name == "host_rdma") return smem_bm::kDataOpHostRdma;
    if (name == "host_tcp") return smem_bm::kDataOpHostTcp;
    if (name == "device_rdma") return smem_bm::kDataOpDeviceRdma;
    if (name == "host_urma") return smem_bm::kDataOpHostUrma;
    if (name == "host_shm") return smem_bm::kDataOpHostShm;
    if (name == "device_urma") return smem_bm::kDataOpDeviceUrma;
    if (name == "device_uboe") return smem_bm::kDataOpDeviceUboe;
    LOG(ERROR) << "Invalid MC_MEMORY_ENGINE_DATA_OP: " << name;
    return 0;
}

Status SmemStatus(int32_t ret, const char* operation) {
    std::string message = std::string(operation) + " failed with " +
                          std::to_string(ret) + LOC_MARK;
    switch (ret) {
        case smem_bm::kInvalidParam:
            return Status::InvalidArgument(message);
        case smem_bm::kTimeout:
            return Status::MemoryEngineTimeout(message);
        case smem_bm::kNotInitialized:
            return Status::MemoryEngineNotJoined(message);
        default:
            return Status::MemoryEngineBackendError(message);
    }
}

}  // namespace

MemfabricBackend::~MemfabricBackend() { shutdown(); }

int MemfabricBackend::typeIndex(MemoryEngineMemType type) {
    switch (type) {
        case MemoryEngineMemType::Host:
            return 0;
        case MemoryEngineMemType::Device:
            return 1;
    }
    return -1;
}

smem_bm::SmemMemType MemfabricBackend::toSmemMemType(MemoryEngineMemType type) {
    return type == MemoryEngineMemType::Device ? smem_bm::kGlobalDevice
                                               : smem_bm::kGlobalHost;
}

Status MemfabricBackend::create(const MemoryEngineConfig& config) {
    std::lock_guard<std::mutex> lock(instance_mutex_);
    if (instance_active_) {
        return Status::InvalidArgument(
            "this MemfabricBackend is already created" LOC_MARK);
    }
    if (config.store_url.empty()) {
        return Status::InvalidArgument(
            "memory_engine/store_url is required for the MemoryEngine "
            "control plane" LOC_MARK);
    }

    config_ = config;
    data_op_type_ = ParseDataOpType();
    if (data_op_type_ == 0) {
        return Status::InvalidArgument(
            "invalid MC_MEMORY_ENGINE_DATA_OP" LOC_MARK);
    }
    CHECK_STATUS(initializeBackendLocked());
    instance_active_ = true;
    return Status::OK();
}

Status MemfabricBackend::initializeBackendLocked() {
    if (initialized_) return Status::OK();

    if (!SmemBmUnderApi::LoadLibrary()) {
        return Status::DeviceNotFound(
            "libmf_smem.so is unavailable; install MemFabric or set "
            "MC_MEMORY_ENGINE_LIB" LOC_MARK);
    }
    library_loaded_ = true;

    smem_bm::SmemBmConfig bm_config{};
    int32_t ret = SmemBmUnderApi::ConfigInit(&bm_config);
    if (ret != smem_bm::kOk) {
        SmemBmUnderApi::UnloadLibrary();
        library_loaded_ = false;
        return SmemStatus(ret, "smem_bm_config_init");
    }
    bm_config.autoRanking = config_.auto_ranking;
    bm_config.rankId = config_.auto_ranking ? 0 : config_.local_rank;
    bm_config.initTimeout = config_.join_timeout_s;
    bm_config.createTimeout = config_.join_timeout_s;
    bm_config.controlOperationTimeout = config_.join_timeout_s;

    ret = SmemBmUnderApi::Init(config_.store_url.c_str(), config_.world_size,
                               0xffff, &bm_config);
    if (ret != smem_bm::kOk) {
        SmemBmUnderApi::UnloadLibrary();
        library_loaded_ = false;
        return SmemStatus(ret, "smem_bm_init");
    }
    bm_initialized_ = true;

    smem_bm::SmemBmCreateOption option{};
    option.maxDramSize = config_.host_mem_max;
    option.maxHbmSize = config_.device_mem_max;
    option.localDRAMSize = 0;
    option.localHBMSize = 0;
    option.dataOpType = static_cast<smem_bm::SmemDataOpType>(data_op_type_);
    option.enable56BitsGva = false;
    option.flags = 0;
    option.dramShmFd = -1;

    handle_ = SmemBmUnderApi::Create2(0, &option);
    if (!handle_) {
        cleanupBackendLocked();
        return Status::MemoryEngineBackendError(
            "smem_bm_create2 failed" LOC_MARK);
    }

    const uint32_t rank = SmemBmUnderApi::GetRankId();
    if (rank == std::numeric_limits<uint32_t>::max()) {
        cleanupBackendLocked();
        return Status::MemoryEngineBackendError(
            "smem_bm_get_rank_id returned an invalid rank" LOC_MARK);
    }
    local_rank_ = rank;
    initialized_ = true;
    return Status::OK();
}

void MemfabricBackend::cleanupBackendLocked() {
    if (handle_) {
        SmemBmUnderApi::Destroy(handle_);
        handle_ = nullptr;
    }
    if (bm_initialized_) {
        SmemBmUnderApi::Uninit(0);
        bm_initialized_ = false;
    }
    if (library_loaded_) {
        SmemBmUnderApi::UnloadLibrary();
        library_loaded_ = false;
    }
    initialized_ = false;
    joined_ = false;
    local_rank_ = 0;
}

void MemfabricBackend::shutdown() {
    std::lock_guard<std::mutex> lock(instance_mutex_);
    cleanupBackendLocked();
    config_ = MemoryEngineConfig{};
    instance_active_ = false;
}

Status MemfabricBackend::join() {
    if (joined_) {
        return Status::MemoryEngineAlreadyJoined(
            "MemoryEngine is already joined" LOC_MARK);
    }
    if (!initialized_) {
        CHECK_STATUS(initializeBackendLocked());
    }
    const int32_t ret = SmemBmUnderApi::Join(handle_, 0);
    if (ret != smem_bm::kOk) return SmemStatus(ret, "smem_bm_join");
    joined_ = true;
    return Status::OK();
}

Status MemfabricBackend::leave() {
    if (!joined_) {
        return Status::MemoryEngineNotJoined(
            "MemoryEngine is not joined" LOC_MARK);
    }
    const int32_t ret = SmemBmUnderApi::Leave(handle_, 0);
    cleanupBackendLocked();
    if (ret != smem_bm::kOk) return SmemStatus(ret, "smem_bm_leave");
    return Status::OK();
}

Status MemfabricBackend::getLocalRank(uint32_t* rank_out) const {
    if (!rank_out) {
        return Status::InvalidArgument("rank_out is null" LOC_MARK);
    }
    if (!initialized_) {
        return Status::MemoryEngineNotJoined(
            "MemoryEngine is not initialized" LOC_MARK);
    }
    *rank_out = local_rank_;
    return Status::OK();
}

Status MemfabricBackend::gvaBase(MemoryEngineMemType type, uint32_t rank,
                                 void** gva_out) const {
    if (!gva_out) {
        return Status::InvalidArgument("gva_out is null" LOC_MARK);
    }
    *gva_out = nullptr;
    if (!initialized_) {
        return Status::MemoryEngineNotJoined(
            "MemoryEngine is not initialized" LOC_MARK);
    }
    if (typeIndex(type) < 0) {
        return Status::InvalidArgument("invalid memory type" LOC_MARK);
    }
    if (rank > std::numeric_limits<uint16_t>::max()) {
        return Status::InvalidArgument("rank is out of range" LOC_MARK);
    }
    void* base = SmemBmUnderApi::PtrByMemType(handle_, toSmemMemType(type),
                                              static_cast<uint16_t>(rank));
    if (!base) {
        return Status::InvalidArgument(
            "rank is out of range or the memory type is not "
            "configured" LOC_MARK);
    }
    *gva_out = base;
    return Status::OK();
}

Status MemfabricBackend::rankOfGva(const void* gva, uint32_t* rank_out) const {
    if (!gva || !rank_out) {
        return Status::InvalidArgument(
            "gva and rank_out are required" LOC_MARK);
    }
    if (!initialized_) {
        return Status::MemoryEngineNotJoined(
            "MemoryEngine is not initialized" LOC_MARK);
    }
    const uint32_t rank =
        SmemBmUnderApi::GetRankIdByGva(handle_, const_cast<void*>(gva));
    if (rank == std::numeric_limits<uint32_t>::max()) {
        return Status::InvalidArgument(
            "gva does not belong to this MemoryEngine pool" LOC_MARK);
    }
    *rank_out = rank;
    return Status::OK();
}

Status MemfabricBackend::allocate(uint64_t, MemoryEngineMemType, void**) {
    return Status::MemoryEngineUnsupported(
        "allocate is implemented in a follow-up change" LOC_MARK);
}

Status MemfabricBackend::free(void*) {
    return Status::MemoryEngineUnsupported(
        "free is implemented in a follow-up change" LOC_MARK);
}

Status MemfabricBackend::batchCopy(const std::vector<MemoryEngineCopyEntry>&,
                                   std::vector<int32_t>*) {
    return Status::MemoryEngineUnsupported(
        "batchCopy is implemented in a follow-up change" LOC_MARK);
}

}  // namespace tent
}  // namespace mooncake
