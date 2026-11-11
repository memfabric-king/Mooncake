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

int32_t SmemToMemoryEngineError(int32_t ret) {
    switch (ret) {
        case smem_bm::kOk:
            return MEMORY_ENGINE_SUCCESS;
        case smem_bm::kInvalidParam:
            return MEMORY_ENGINE_E_INVALID_ARG;
        case smem_bm::kTimeout:
            return MEMORY_ENGINE_E_TIMEOUT;
        case smem_bm::kNotInitialized:
            return MEMORY_ENGINE_E_NOT_JOINED;
        default:
            return MEMORY_ENGINE_E_BACKEND;
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

Status MemfabricBackend::allocate(uint64_t size, MemoryEngineMemType type,
                                  void** gva_out) {
    if (!gva_out) {
        return Status::InvalidArgument("gva_out is null" LOC_MARK);
    }
    *gva_out = nullptr;
    if (!joined_) {
        return Status::MemoryEngineNotJoined(
            "MemoryEngine must be joined before allocate" LOC_MARK);
    }

    const auto mem_type = toSmemMemType(type);
    const uint64_t offset = SmemBmUnderApi::GetLocalMemSize(handle_, mem_type);
    const int32_t ret = SmemBmUnderApi::ExtendLocalMem(handle_, mem_type, size);
    if (ret != smem_bm::kOk) {
        return SmemStatus(ret, "smem_bm_extend_local_mem");
    }

    void* base = SmemBmUnderApi::PtrByMemType(
        handle_, mem_type, static_cast<uint16_t>(local_rank_));
    if (!base) {
        return Status::MemoryEngineBackendError(
            "failed to resolve the local GVA base" LOC_MARK);
    }
    *gva_out = static_cast<uint8_t*>(base) + offset;
    return Status::OK();
}

Status MemfabricBackend::free(void* gva) {
    (void)gva;
    return Status::OK();
}

Status MemfabricBackend::batchCopy(
    const std::vector<MemoryEngineCopyEntry>& entries,
    std::vector<int32_t>* results) {
    if (!results) {
        return Status::InvalidArgument("results is null" LOC_MARK);
    }
    results->assign(entries.size(), MEMORY_ENGINE_SUCCESS);
    if (!joined_) {
        for (auto& result : *results) result = MEMORY_ENGINE_E_NOT_JOINED;
        return Status::MemoryEngineNotJoined(
            "MemoryEngine must be joined before batchCopy" LOC_MARK);
    }
    if (entries.empty()) return Status::OK();
    if (entries.size() > std::numeric_limits<uint32_t>::max()) {
        for (auto& result : *results) result = MEMORY_ENGINE_E_INVALID_ARG;
        return Status::InvalidArgument(
            "batchCopy entry count exceeds the backend limit" LOC_MARK);
    }

    std::vector<void*> sources;
    std::vector<void*> destinations;
    std::vector<uint64_t> sizes;
    std::vector<size_t> indices;
    sources.reserve(entries.size());
    destinations.reserve(entries.size());
    sizes.reserve(entries.size());
    indices.reserve(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        if (!entry.src || !entry.dst || entry.size == 0) {
            (*results)[i] = MEMORY_ENGINE_E_INVALID_ARG;
            continue;
        }
        sources.push_back(const_cast<void*>(entry.src));
        destinations.push_back(entry.dst);
        sizes.push_back(entry.size);
        indices.push_back(i);
    }
    if (indices.empty()) return Status::OK();

    smem_bm::SmemBatchCopyParams params{
        sources.data(), destinations.data(), sizes.data(),
        static_cast<uint32_t>(sources.size()), nullptr};
    const int32_t ret =
        SmemBmUnderApi::CopyBatch(handle_, &params, smem_bm::kCopyAuto, 0);
    if (ret == smem_bm::kOk) return Status::OK();

    // The vendor batch call reports one status for the whole batch. Retry each
    // entry on its own so a single bad address does not hide the successes.
    LOG(WARNING) << "smem_bm_copy_batch failed with " << ret
                 << "; retrying entries individually to attribute failures";
    bool any_success = false;
    for (size_t i = 0; i < indices.size(); ++i) {
        void* source = sources[i];
        void* destination = destinations[i];
        uint64_t size = sizes[i];
        smem_bm::SmemBatchCopyParams single{&source, &destination, &size, 1,
                                            nullptr};
        const int32_t entry_ret =
            SmemBmUnderApi::CopyBatch(handle_, &single, smem_bm::kCopyAuto, 0);
        (*results)[indices[i]] = SmemToMemoryEngineError(entry_ret);
        any_success = any_success || entry_ret == smem_bm::kOk;
    }
    // Per-entry problems (bad addresses) belong in results[] and do not make
    // the call itself fail. When every entry failed and the batch error is a
    // lifecycle/backend problem, surface it as the overall status as well.
    if (!any_success && ret != smem_bm::kInvalidParam) {
        return SmemStatus(ret, "smem_bm_copy_batch");
    }
    return Status::OK();
}

}  // namespace tent
}  // namespace mooncake
