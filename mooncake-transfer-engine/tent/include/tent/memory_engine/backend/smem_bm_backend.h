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

#ifndef TENT_MEMORY_ENGINE_SMEM_BM_BACKEND_H
#define TENT_MEMORY_ENGINE_SMEM_BM_BACKEND_H

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "smem_bm_api.h"
#include "tent/memory_engine.h"

namespace mooncake {
namespace tent {

// Backend that owns the GVA layout, the local allocator, and the smem_bm
// lifecycle. The facade above it only validates arguments and serializes
// calls; all address arithmetic lives here.
class SmemBmBackend {
   public:
    SmemBmBackend() = default;
    ~SmemBmBackend();

    SmemBmBackend(const SmemBmBackend&) = delete;
    SmemBmBackend& operator=(const SmemBmBackend&) = delete;

    // Loads libmf_smem.so, initializes the config store, and creates the local
    // Big Memory segment (but does not join the global pool). The control-plane
    // URL is taken from config.store_url. smem_bm_init and smem_bm_create2 run
    // here so the rank and GVA bases are available as soon as create()
    // returns; join() publishes the rank to the global pool. smem_bm_init() is
    // process-global, so callers must not create backends with conflicting
    // world sizes.
    Status create(const MemoryEngineConfig& config);

    Status join();
    Status leave();
    void shutdown();

    bool initialized() const { return initialized_; }
    bool joined() const { return joined_; }

    Status getLocalRank(uint32_t* rank_out) const;
    Status gvaBase(MemoryEngineMemType type, uint32_t rank,
                   void** gva_out) const;
    Status rankOfGva(const void* gva, uint32_t* rank_out) const;

    Status allocate(uint64_t size, MemoryEngineMemType type, void** gva_out);
    Status free(void* gva);
    Status batchCopy(const std::vector<MemoryEngineCopyEntry>& entries,
                     std::vector<int32_t>* results);

   private:
    Status initializeBackendLocked();
    void cleanupBackendLocked();

    static int typeIndex(MemoryEngineMemType type);
    static smem_bm::SmemMemType toSmemMemType(MemoryEngineMemType type);

    MemoryEngineConfig config_;
    uint16_t device_id_{0};
    int32_t data_op_type_{0};

    smem_bm::SmemBmHandle handle_{nullptr};
    bool library_loaded_{false};
    bool bm_initialized_{false};
    bool initialized_{false};
    bool joined_{false};
    uint32_t local_rank_{0};

    std::mutex instance_mutex_;
    bool instance_active_{false};
};

}  // namespace tent
}  // namespace mooncake

#endif  // TENT_MEMORY_ENGINE_SMEM_BM_BACKEND_H
