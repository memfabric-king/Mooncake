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

#include "smem_bm_backend.h"

#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

#include <glog/logging.h>

namespace mooncake {
namespace tent {
namespace {

Status AlignUp(uint64_t value, uint64_t* out) {
    if (!out) return Status::InvalidArgument("null output" LOC_MARK);
    if (value == 0) {
        *out = 0;
        return Status::OK();
    }
    constexpr uint64_t mask = smem_bm::kLargePageSize - 1;
    if (value > std::numeric_limits<uint64_t>::max() - mask) {
        return Status::InvalidArgument(
            "memory size overflows the backend alignment" LOC_MARK);
    }
    *out = (value + mask) & ~mask;
    return Status::OK();
}

uint32_t TimeoutSeconds(int64_t timeout_ms) {
    if (timeout_ms <= 0) return 1;
    uint64_t seconds = (static_cast<uint64_t>(timeout_ms) + 999ULL) / 1000ULL;
    if (seconds == 0) seconds = 1;
    if (seconds > std::numeric_limits<uint32_t>::max())
        seconds = std::numeric_limits<uint32_t>::max();
    return static_cast<uint32_t>(seconds);
}

int32_t ParseDataOpType() {
    const char* value = std::getenv("MC_MEMORY_ENGINE_DATA_OP");
    const std::string name = (value && *value) ? value : "device_rdma";
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

uint16_t ParseDeviceId() {
    const char* value = std::getenv("MC_MEMORY_ENGINE_DEVICE_ID");
    if (!value || !*value) return 0;
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || (end && *end != '\0') || parsed < 0 ||
        parsed > std::numeric_limits<uint16_t>::max()) {
        LOG(WARNING) << "Invalid MC_MEMORY_ENGINE_DEVICE_ID: " << value
                     << ", using 0";
        return 0;
    }
    return static_cast<uint16_t>(parsed);
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

std::mutex SmemBmBackend::instance_mutex_;
bool SmemBmBackend::instance_active_ = false;

SmemBmBackend::~SmemBmBackend() { shutdown(); }

int SmemBmBackend::typeIndex(MemoryEngineMemType type) {
    switch (type) {
        case MemoryEngineMemType::Host:
            return 0;
        case MemoryEngineMemType::Device:
            return 1;
    }
    return -1;
}

smem_bm::SmemMemType SmemBmBackend::toSmemMemType(MemoryEngineMemType type) {
    return type == MemoryEngineMemType::Device ? smem_bm::kGlobalDevice
                                               : smem_bm::kGlobalHost;
}

Status SmemBmBackend::prepareLayoutLocked() {
    if (!config_.enabled) {
        return Status::MemoryEngineDisabled(
            "memory_engine/enabled is false" LOC_MARK);
    }
    if (config_.world_size == 0) {
        return Status::InvalidArgument(
            "memory_engine/world_size must be greater than 0" LOC_MARK);
    }
    if (!config_.auto_ranking && config_.local_rank >= config_.world_size) {
        return Status::InvalidArgument(
            "memory_engine/local_rank must be smaller than "
            "world_size" LOC_MARK);
    }
    if (config_.join_timeout_ms <= 0) {
        return Status::InvalidArgument(
            "memory_engine/join_timeout_ms must be greater than 0" LOC_MARK);
    }

    uint64_t host_max = 0;
    uint64_t device_max = 0;
    CHECK_STATUS(AlignUp(config_.host_mem_max, &host_max));
    CHECK_STATUS(AlignUp(config_.device_mem_max, &device_max));
    if (host_max == 0 && device_max == 0) {
        return Status::InvalidArgument(
            "memory_engine/host_mem_max and device_mem_max cannot both be "
            "zero" LOC_MARK);
    }
    if (host_max > smem_bm::kMaxLocalDramSize) {
        return Status::InvalidArgument(
            "memory_engine/host_mem_max exceeds the backend limit" LOC_MARK);
    }
    if (device_max > smem_bm::kMaxLocalHbmSize) {
        return Status::InvalidArgument(
            "memory_engine/device_mem_max exceeds the backend limit" LOC_MARK);
    }

    const uint64_t world_size = config_.world_size;
    if (host_max > std::numeric_limits<uint64_t>::max() / world_size ||
        device_max > std::numeric_limits<uint64_t>::max() / world_size) {
        return Status::InvalidArgument(
            "GVA layout size overflows 64 bits" LOC_MARK);
    }

    world_size_ = config_.world_size;
    max_[0] = host_max;
    max_[1] = device_max;
    total_[0] = host_max * world_size;
    total_[1] = device_max * world_size;
    return Status::OK();
}

Status SmemBmBackend::create(const std::string& store_url,
                             const MemoryEngineConfig& config) {
    std::lock_guard<std::mutex> lock(instance_mutex_);
    if (instance_active_) {
        return Status::InvalidArgument(
            "another MemoryEngine is already active in this process" LOC_MARK);
    }
    if (store_url.empty()) {
        return Status::InvalidArgument(
            "metadata_servers is required for the MemoryEngine control "
            "plane" LOC_MARK);
    }

    config_ = config;
    store_url_ = store_url;
    data_op_type_ = ParseDataOpType();
    if (data_op_type_ == 0) {
        return Status::InvalidArgument(
            "invalid MC_MEMORY_ENGINE_DATA_OP" LOC_MARK);
    }
    device_id_ = ParseDeviceId();

    CHECK_STATUS(prepareLayoutLocked());
    CHECK_STATUS(initializeBackendLocked());
    instance_active_ = true;
    owns_instance_ = true;
    return Status::OK();
}

Status SmemBmBackend::initializeBackendLocked() {
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
    const uint32_t timeout_s = TimeoutSeconds(config_.join_timeout_ms);
    bm_config.initTimeout = timeout_s;
    bm_config.createTimeout = timeout_s;
    bm_config.controlOperationTimeout = timeout_s;

    ret = SmemBmUnderApi::Init(store_url_.c_str(), world_size_, device_id_,
                               &bm_config);
    if (ret != smem_bm::kOk) {
        SmemBmUnderApi::UnloadLibrary();
        library_loaded_ = false;
        return SmemStatus(ret, "smem_bm_init");
    }
    bm_initialized_ = true;

    smem_bm::SmemBmCreateOption option{};
    option.maxDramSize = max_[0];
    option.maxHbmSize = max_[1];
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
    if (rank == std::numeric_limits<uint32_t>::max() || rank >= world_size_) {
        cleanupBackendLocked();
        return Status::MemoryEngineBackendError(
            "smem_bm_get_rank_id returned an invalid rank" LOC_MARK);
    }
    if (!config_.auto_ranking && rank != config_.local_rank) {
        cleanupBackendLocked();
        return Status::InvalidArgument(
            "smem_bm assigned a rank different from "
            "memory_engine/local_rank" LOC_MARK);
    }
    local_rank_ = rank;

    if (max_[0] > 0) {
        base_[0] =
            SmemBmUnderApi::PtrByMemType(handle_, smem_bm::kGlobalHost, 0);
        if (!base_[0]) {
            cleanupBackendLocked();
            return Status::MemoryEngineBackendError(
                "failed to resolve the host GVA base" LOC_MARK);
        }
    }
    if (max_[1] > 0) {
        base_[1] =
            SmemBmUnderApi::PtrByMemType(handle_, smem_bm::kGlobalDevice, 0);
        if (!base_[1]) {
            cleanupBackendLocked();
            return Status::MemoryEngineBackendError(
                "failed to resolve the device GVA base" LOC_MARK);
        }
    }

    initialized_ = true;
    return Status::OK();
}

void SmemBmBackend::cleanupBackendLocked() {
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
    base_[0] = nullptr;
    base_[1] = nullptr;
    resetAllocatorLocked();
}

void SmemBmBackend::resetAllocatorLocked() {
    for (auto& blocks : free_blocks_) blocks.clear();
    allocations_.clear();
    extended_[0] = 0;
    extended_[1] = 0;
}

void SmemBmBackend::resetLayoutLocked() {
    max_[0] = 0;
    max_[1] = 0;
    total_[0] = 0;
    total_[1] = 0;
    world_size_ = 0;
    local_rank_ = 0;
}

void SmemBmBackend::shutdown() {
    std::lock_guard<std::mutex> lock(instance_mutex_);
    cleanupBackendLocked();
    resetLayoutLocked();
    config_ = MemoryEngineConfig{};
    store_url_.clear();
    if (owns_instance_) {
        instance_active_ = false;
        owns_instance_ = false;
    }
}

Status SmemBmBackend::join() {
    if (joined_) {
        return Status::MemoryEngineAlreadyJoined(
            "MemoryEngine is already joined" LOC_MARK);
    }
    if (!initialized_) {
        CHECK_STATUS(prepareLayoutLocked());
        CHECK_STATUS(initializeBackendLocked());
    }
    const int32_t ret = SmemBmUnderApi::Join(handle_, 0);
    if (ret != smem_bm::kOk) return SmemStatus(ret, "smem_bm_join");
    joined_ = true;
    return Status::OK();
}

Status SmemBmBackend::leave() {
    if (!joined_) {
        return Status::MemoryEngineNotJoined(
            "MemoryEngine is not joined" LOC_MARK);
    }
    if (!allocations_.empty()) {
        LOG(WARNING) << "MemoryEngine::leave called with "
                     << allocations_.size()
                     << " live allocation(s); their GVAs become invalid";
    }
    const int32_t ret = SmemBmUnderApi::Leave(handle_, 0);
    cleanupBackendLocked();
    if (ret != smem_bm::kOk) return SmemStatus(ret, "smem_bm_leave");
    return Status::OK();
}

Status SmemBmBackend::getLocalRank(uint32_t* rank_out) const {
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

Status SmemBmBackend::gvaBase(MemoryEngineMemType type, uint32_t rank,
                              void** gva_out) const {
    if (!gva_out) {
        return Status::InvalidArgument("gva_out is null" LOC_MARK);
    }
    *gva_out = nullptr;
    if (!initialized_) {
        return Status::MemoryEngineNotJoined(
            "MemoryEngine is not initialized" LOC_MARK);
    }
    const int index = typeIndex(type);
    if (index < 0) {
        return Status::InvalidArgument("invalid memory type" LOC_MARK);
    }
    if (rank >= world_size_) {
        return Status::InvalidArgument("rank is out of range" LOC_MARK);
    }
    if (max_[index] == 0 || base_[index] == nullptr) {
        return Status::InvalidArgument(
            "the requested memory type is not configured" LOC_MARK);
    }
    *gva_out = static_cast<uint8_t*>(base_[index]) +
               static_cast<uint64_t>(rank) * max_[index];
    return Status::OK();
}

Status SmemBmBackend::rankOfGva(const void* gva, uint32_t* rank_out) const {
    if (!gva || !rank_out) {
        return Status::InvalidArgument(
            "gva and rank_out are required" LOC_MARK);
    }
    if (!initialized_) {
        return Status::MemoryEngineNotJoined(
            "MemoryEngine is not initialized" LOC_MARK);
    }
    const uint64_t address = reinterpret_cast<uint64_t>(gva);
    for (int index = 0; index < 2; ++index) {
        if (max_[index] == 0 || base_[index] == nullptr) continue;
        const uint64_t start = reinterpret_cast<uint64_t>(base_[index]);
        if (address < start) continue;
        const uint64_t delta = address - start;
        if (delta < total_[index]) {
            *rank_out = static_cast<uint32_t>(delta / max_[index]);
            return Status::OK();
        }
    }
    return Status::InvalidArgument(
        "gva does not belong to this MemoryEngine pool" LOC_MARK);
}

Status SmemBmBackend::allocate(uint64_t size, MemoryEngineMemType type,
                               void** gva_out) {
    if (!gva_out) {
        return Status::InvalidArgument("gva_out is null" LOC_MARK);
    }
    *gva_out = nullptr;
    if (!joined_) {
        return Status::MemoryEngineNotJoined(
            "MemoryEngine must be joined before allocate" LOC_MARK);
    }
    const int index = typeIndex(type);
    if (index < 0) {
        return Status::InvalidArgument("invalid memory type" LOC_MARK);
    }
    uint64_t aligned = 0;
    CHECK_STATUS(AlignUp(size, &aligned));
    if (aligned == 0) {
        return Status::InvalidArgument(
            "allocation size must be greater than 0" LOC_MARK);
    }
    if (max_[index] == 0 || base_[index] == nullptr) {
        return Status::MemoryEngineNoSpace(
            "the requested memory type is not configured" LOC_MARK);
    }
    if (aligned > max_[index]) {
        return Status::MemoryEngineNoSpace(
            "allocation exceeds the per-rank memory window" LOC_MARK);
    }

    uint64_t offset = 0;
    auto& blocks = free_blocks_[index];
    auto block = blocks.begin();
    for (; block != blocks.end(); ++block) {
        if (block->second >= aligned) break;
    }
    if (block != blocks.end()) {
        offset = block->first;
        const uint64_t block_size = block->second;
        blocks.erase(block);
        if (block_size > aligned) {
            blocks.emplace(offset + aligned, block_size - aligned);
        }
    } else {
        if (extended_[index] > max_[index] - aligned) {
            return Status::MemoryEngineNoSpace(
                "no space left in the per-rank memory window" LOC_MARK);
        }
        // smem_bm appends the new slice at the end of this rank's window. The
        // call returns after the server accepts it; peers receive the update
        // asynchronously, so an application that immediately lets another
        // rank access the new GVA needs its own barrier.
        const int32_t ret = SmemBmUnderApi::ExtendLocalMem(
            handle_, toSmemMemType(type), aligned);
        if (ret != smem_bm::kOk) {
            return SmemStatus(ret, "smem_bm_extend_local_mem");
        }
        offset = extended_[index];
        extended_[index] += aligned;
    }

    void* gva = static_cast<uint8_t*>(base_[index]) + offset;
    allocations_[gva] =
        Allocation{offset, aligned, static_cast<uint8_t>(index)};
    *gva_out = gva;
    return Status::OK();
}

Status SmemBmBackend::free(void* gva) {
    if (!gva) {
        return Status::InvalidArgument("gva is null" LOC_MARK);
    }
    if (!joined_) {
        return Status::MemoryEngineNotJoined(
            "MemoryEngine must be joined before free" LOC_MARK);
    }
    auto it = allocations_.find(gva);
    if (it == allocations_.end()) {
        return Status::InvalidArgument(
            "gva was not returned by MemoryEngine::allocate" LOC_MARK);
    }
    // smem_bm has no shrink or unmap entry point. Return the block to the
    // local free list so a later allocate() can reuse the same GVA; the
    // physical slice stays mapped until leave() destroys the BM.
    const Allocation allocation = it->second;
    allocations_.erase(it);

    const int index = allocation.type;
    if (index < 0 || index > 1) {
        return Status::InternalError("corrupt allocation type" LOC_MARK);
    }
    auto& blocks = free_blocks_[index];
    uint64_t offset = allocation.offset;
    uint64_t size = allocation.size;

    auto next = blocks.lower_bound(offset);
    if (next != blocks.end() && next->first == offset + size) {
        size += next->second;
        blocks.erase(next);
    }
    auto previous = blocks.lower_bound(offset);
    if (previous != blocks.begin()) {
        --previous;
        if (previous->first + previous->second == offset) {
            offset = previous->first;
            size += previous->second;
            blocks.erase(previous);
        }
    }
    blocks.emplace(offset, size);
    return Status::OK();
}

Status SmemBmBackend::batchCopy(
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
