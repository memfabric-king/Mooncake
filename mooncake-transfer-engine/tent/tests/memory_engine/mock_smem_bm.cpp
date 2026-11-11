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

#include "tent/memory_engine/backend/smem_bm_abi.h"

namespace {

using namespace mooncake::tent::smem_bm;

struct State {
    bool initialized{false};
    bool joined{false};
    uint32_t world_size{0};
    uint32_t rank{0};
    uint64_t max_host{0};
    uint64_t max_device{0};
    void* host_map{nullptr};
    void* device_map{nullptr};
    size_t host_length{0};
    size_t device_length{0};
    void* host_base{nullptr};
    void* device_base{nullptr};
    void* token{nullptr};
};

std::mutex g_mutex;
State g_state;
void UnmapAll() {
    if (g_state.host_map) munmap(g_state.host_map, g_state.host_length);
    if (g_state.device_map) munmap(g_state.device_map, g_state.device_length);
    g_state.host_map = nullptr;
    g_state.device_map = nullptr;
    g_state.host_base = nullptr;
    g_state.device_base = nullptr;
}

void ResetLocked() {
    UnmapAll();
    g_state = State{};
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

int MemTypeIndex(int32_t mem_type) {
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

int32_t smem_bm_init(const char* store_url, uint32_t world_size, uint16_t,
                     const SmemBmConfig* config) {
    if (!store_url || !*store_url || world_size == 0 || !config)
        return kInvalidParam;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_state.initialized) return kOk;
    uint32_t rank = 0;
    if (!config->autoRanking) {
        rank = config->rankId;
        if (rank >= world_size) return kInvalidParam;
    }
    g_state = State{};
    g_state.initialized = true;
    g_state.world_size = world_size;
    g_state.rank = rank;
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
    if (!g_state.initialized || g_state.token) return g_state.token;
    if (option->maxDramSize == 0 && option->maxHbmSize == 0) return nullptr;
    size_t host_length = 0;
    size_t device_length = 0;
    void* host_map = nullptr;
    void* device_map = nullptr;
    void* host_base = nullptr;
    void* device_base = nullptr;
    if (!MapRegion(option->maxDramSize, g_state.world_size, &host_length,
                   &host_map, &host_base)) {
        return nullptr;
    }
    if (!MapRegion(option->maxHbmSize, g_state.world_size, &device_length,
                   &device_map, &device_base)) {
        if (host_map) munmap(host_map, host_length);
        return nullptr;
    }
    g_state.max_host = option->maxDramSize;
    g_state.max_device = option->maxHbmSize;
    g_state.host_map = host_map;
    g_state.device_map = device_map;
    g_state.host_length = host_length;
    g_state.device_length = device_length;
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

void* smem_bm_ptr_by_mem_type(SmemBmHandle handle, SmemMemType mem_type,
                              uint16_t peer_rank) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!handle || handle != g_state.token) return nullptr;
    if (peer_rank >= g_state.world_size) return nullptr;
    const int index = MemTypeIndex(mem_type);
    if (index < 0) return nullptr;
    void* base = index == 0 ? g_state.host_base : g_state.device_base;
    const uint64_t max = index == 0 ? g_state.max_host : g_state.max_device;
    if (!base || max == 0) return nullptr;
    return static_cast<uint8_t*>(base) + static_cast<uint64_t>(peer_rank) * max;
}

uint32_t smem_bm_get_rank_id_by_gva(SmemBmHandle handle, void* gva) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_state.initialized || !handle || handle != g_state.token || !gva)
        return std::numeric_limits<uint32_t>::max();
    const uint64_t address = reinterpret_cast<uint64_t>(gva);
    if (g_state.host_base && g_state.max_host) {
        const uint64_t start = reinterpret_cast<uint64_t>(g_state.host_base);
        if (address >= start &&
            address < start + g_state.max_host * g_state.world_size) {
            return static_cast<uint32_t>((address - start) / g_state.max_host);
        }
    }
    if (g_state.device_base && g_state.max_device) {
        const uint64_t start = reinterpret_cast<uint64_t>(g_state.device_base);
        if (address >= start &&
            address < start + g_state.max_device * g_state.world_size) {
            return static_cast<uint32_t>((address - start) /
                                         g_state.max_device);
        }
    }
    return std::numeric_limits<uint32_t>::max();
}

}  // extern "C"
