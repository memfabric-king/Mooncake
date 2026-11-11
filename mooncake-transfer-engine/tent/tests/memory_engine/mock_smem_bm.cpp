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

#include "mock_smem_bm.h"

#include <sys/mman.h>

#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <string>

#include "tent/memory_engine/backend/smem_bm_abi.h"

namespace {

using namespace mooncake::tent::smem_bm;

struct MockState {
    bool initialized{false};
    bool joined{false};
    bool auto_ranking{true};
    uint32_t world_size{0};
    uint16_t device_id{0};
    uint32_t rank{0};
    uint32_t configured_rank{0};
    uint64_t max_host{0};
    uint64_t max_device{0};
    uint64_t local_host{0};
    uint64_t local_device{0};
    uint64_t extend_calls[2]{0, 0};
    void* host_map_base{nullptr};
    void* device_map_base{nullptr};
    size_t host_map_length{0};
    size_t device_map_length{0};
    void* host_base{nullptr};
    void* device_base{nullptr};
    void* token{nullptr};
    std::string store_url;
};

std::mutex g_mutex;
MockState g_state;

void UnmapAll() {
    if (g_state.host_map_base) {
        munmap(g_state.host_map_base, g_state.host_map_length);
        g_state.host_map_base = nullptr;
        g_state.host_map_length = 0;
    }
    if (g_state.device_map_base) {
        munmap(g_state.device_map_base, g_state.device_map_length);
        g_state.device_map_base = nullptr;
        g_state.device_map_length = 0;
    }
    g_state.host_base = nullptr;
    g_state.device_base = nullptr;
}

void ResetLocked() {
    UnmapAll();
    g_state = MockState{};
}

bool MapRegion(uint64_t max_size, uint32_t world_size, size_t* length,
               void** map_base, void** aligned_base) {
    *length = 0;
    *map_base = nullptr;
    *aligned_base = nullptr;
    if (max_size == 0) return true;
    if (max_size > std::numeric_limits<size_t>::max() / world_size)
        return false;
    const size_t bytes = static_cast<size_t>(max_size) * world_size;
    const size_t allocation = bytes + kLargePageSize;
    void* address = mmap(nullptr, allocation, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (address == MAP_FAILED) return false;
    const uintptr_t aligned =
        (reinterpret_cast<uintptr_t>(address) + kLargePageSize - 1) &
        ~static_cast<uintptr_t>(kLargePageSize - 1);
    *length = allocation;
    *map_base = address;
    *aligned_base = reinterpret_cast<void*>(aligned);
    return true;
}

int32_t MemTypeIndex(int32_t mem_type) {
    if (mem_type == kGlobalHost) return 0;
    if (mem_type == kGlobalDevice) return 1;
    return -1;
}

}  // namespace

extern "C" {

void mock_smem_bm_reset() {
    std::lock_guard<std::mutex> lock(g_mutex);
    ResetLocked();
}

uint64_t mock_smem_bm_local_size(int mem_type) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return mem_type == 1 ? g_state.local_device : g_state.local_host;
}

uint64_t mock_smem_bm_extend_calls(int mem_type) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return mem_type == 1 ? g_state.extend_calls[1] : g_state.extend_calls[0];
}

uint32_t mock_smem_bm_rank() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state.rank;
}

int mock_smem_bm_joined() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state.joined ? 1 : 0;
}

int mock_smem_bm_initialized() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state.initialized ? 1 : 0;
}

int32_t smem_bm_config_init(SmemBmConfig* config) {
    if (!config) return kInvalidParam;
    *config = SmemBmConfig{};
    config->initTimeout = 120;
    config->createTimeout = 120;
    config->controlOperationTimeout = 120;
    config->startConfigStoreServer = true;
    config->autoRanking = true;
    return kOk;
}

int32_t smem_bm_init(const char* store_url, uint32_t world_size,
                     uint16_t device_id, const SmemBmConfig* config) {
    if (!store_url || !*store_url || world_size == 0 || !config)
        return kInvalidParam;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_state.initialized) {
        g_state.store_url = store_url;
        return kOk;
    }
    uint32_t rank = 0;
    if (!config->autoRanking) {
        rank = config->rankId;
        if (rank >= world_size) return kInvalidParam;
    }
    g_state.initialized = true;
    g_state.world_size = world_size;
    g_state.device_id = device_id;
    g_state.auto_ranking = config->autoRanking;
    g_state.configured_rank = config->rankId;
    g_state.rank = rank;
    g_state.store_url = store_url;
    return kOk;
}

void smem_bm_uninit(uint32_t) {
    std::lock_guard<std::mutex> lock(g_mutex);
    ResetLocked();
}

uint32_t smem_bm_get_rank_id() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state.initialized ? g_state.rank
                               : std::numeric_limits<uint32_t>::max();
}

SmemBmHandle smem_bm_create2(uint32_t, const SmemBmCreateOption* option) {
    if (!option) return nullptr;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_state.initialized) return nullptr;
    if (g_state.token) return g_state.token;
    if (option->maxDramSize == 0 && option->maxHbmSize == 0) return nullptr;
    if (option->localDRAMSize > option->maxDramSize ||
        option->localHBMSize > option->maxHbmSize)
        return nullptr;

    size_t host_length = 0;
    size_t device_length = 0;
    void* host_map_base = nullptr;
    void* device_map_base = nullptr;
    void* host_base = nullptr;
    void* device_base = nullptr;
    if (!MapRegion(option->maxDramSize, g_state.world_size, &host_length,
                   &host_map_base, &host_base)) {
        return nullptr;
    }
    if (!MapRegion(option->maxHbmSize, g_state.world_size, &device_length,
                   &device_map_base, &device_base)) {
        if (host_map_base) munmap(host_map_base, host_length);
        return nullptr;
    }

    g_state.max_host = option->maxDramSize;
    g_state.max_device = option->maxHbmSize;
    g_state.local_host = option->localDRAMSize;
    g_state.local_device = option->localHBMSize;
    g_state.host_map_base = host_map_base;
    g_state.device_map_base = device_map_base;
    g_state.host_map_length = host_length;
    g_state.device_map_length = device_length;
    g_state.host_base = host_base;
    g_state.device_base = device_base;
    g_state.token = &g_state;
    return g_state.token;
}

void smem_bm_destroy(SmemBmHandle handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!handle || handle != g_state.token) return;
    UnmapAll();
    g_state.token = nullptr;
    g_state.joined = false;
    g_state.max_host = 0;
    g_state.max_device = 0;
    g_state.local_host = 0;
    g_state.local_device = 0;
    g_state.extend_calls[0] = 0;
    g_state.extend_calls[1] = 0;
}

int32_t smem_bm_join(SmemBmHandle handle, uint32_t) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_state.initialized || !handle || handle != g_state.token)
        return kError;
    g_state.joined = true;
    return kOk;
}

int32_t smem_bm_leave(SmemBmHandle handle, uint32_t) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_state.initialized) return kNotInitialized;
    if (!handle || handle != g_state.token) return kError;
    g_state.joined = false;
    return kOk;
}

int32_t smem_bm_extend_local_mem(SmemBmHandle handle, SmemMemType mem_type,
                                 uint64_t size) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!handle || handle != g_state.token) return kError;
    if (!g_state.joined) return kNotInitialized;
    if (size == 0 || (size % kLargePageSize) != 0) return kInvalidParam;
    const int index = MemTypeIndex(mem_type);
    if (index < 0) return kInvalidParam;
    uint64_t& local = index == 0 ? g_state.local_host : g_state.local_device;
    const uint64_t max = index == 0 ? g_state.max_host : g_state.max_device;
    if (size > max || local > max - size) return kInvalidParam;
    local += size;
    ++g_state.extend_calls[index];
    return kOk;
}

uint64_t smem_bm_get_local_mem_size_by_mem_type(SmemBmHandle handle,
                                                SmemMemType mem_type) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!handle || handle != g_state.token) return 0;
    const int index = MemTypeIndex(mem_type);
    if (index < 0) return 0;
    return index == 0 ? g_state.local_host : g_state.local_device;
}

void* smem_bm_ptr_by_mem_type(SmemBmHandle handle, SmemMemType mem_type,
                              uint16_t peer_rank) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!handle || handle != g_state.token) return nullptr;
    if (peer_rank >= g_state.world_size) return nullptr;
    const int index = MemTypeIndex(mem_type);
    if (index < 0) return nullptr;
    void* base = index == 0 ? g_state.host_base : g_state.device_base;
    const uint64_t max = index == 0 ? g_state.max_host : g_state.max_device;
    if (!base) return nullptr;
    return static_cast<uint8_t*>(base) + static_cast<uint64_t>(peer_rank) * max;
}

int32_t smem_bm_copy_batch(SmemBmHandle handle, SmemBatchCopyParams* params,
                           SmemCopyType, uint32_t) {
    if (!params || !params->sources || !params->destinations ||
        !params->dataSizes || params->batchSize == 0)
        return kInvalidParam;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_state.initialized || !g_state.joined) return kNotInitialized;
    if (!handle || handle != g_state.token) return kError;
    for (uint32_t i = 0; i < params->batchSize; ++i) {
        if (!params->sources[i] || !params->destinations[i] ||
            params->dataSizes[i] == 0)
            return kInvalidParam;
        if (reinterpret_cast<uintptr_t>(params->sources[i]) ==
                kMockSmemBmFailAddress ||
            reinterpret_cast<uintptr_t>(params->destinations[i]) ==
                kMockSmemBmFailAddress)
            return kInvalidParam;
    }
    for (uint32_t i = 0; i < params->batchSize; ++i) {
        std::memcpy(params->destinations[i], params->sources[i],
                    static_cast<size_t>(params->dataSizes[i]));
    }
    return kOk;
}

int32_t smem_bm_wait(SmemBmHandle handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_state.initialized || !handle || handle != g_state.token)
        return kNotInitialized;
    return kOk;
}

uint32_t smem_bm_get_rank_id_by_gva(SmemBmHandle handle, void* gva) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_state.initialized || !handle || handle != g_state.token || !gva)
        return std::numeric_limits<uint32_t>::max();
    const uint64_t address = reinterpret_cast<uint64_t>(gva);
    const uint64_t host_start = reinterpret_cast<uint64_t>(g_state.host_base);
    if (g_state.host_base && address >= host_start &&
        address < host_start + g_state.max_host * g_state.world_size) {
        return static_cast<uint32_t>((address - host_start) / g_state.max_host);
    }
    const uint64_t device_start =
        reinterpret_cast<uint64_t>(g_state.device_base);
    if (g_state.device_base && address >= device_start &&
        address < device_start + g_state.max_device * g_state.world_size) {
        return static_cast<uint32_t>((address - device_start) /
                                     g_state.max_device);
    }
    return std::numeric_limits<uint32_t>::max();
}

}  // extern "C"
