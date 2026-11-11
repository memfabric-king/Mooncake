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

#ifndef TENT_MEMORY_ENGINE_SMEM_BM_API_H
#define TENT_MEMORY_ENGINE_SMEM_BM_API_H

#include <cstdint>
#include <mutex>

#include "smem_bm_abi.h"

namespace mooncake {
namespace tent {

// dlopen/dlsym wrapper for libmf_smem.so. MemoryEngine never links MemFabric
// directly; a missing library is reported by create() instead of a load-time
// failure. MC_MEMORY_ENGINE_LIB overrides the default library name and is
// primarily useful for tests or non-standard install prefixes.
class SmemBmUnderApi {
   public:
    static bool LoadLibrary();
    static void UnloadLibrary();

    static int32_t ConfigInit(smem_bm::SmemBmConfig* config);
    static int32_t Init(const char* store_url, uint32_t world_size,
                        uint16_t device_id,
                        const smem_bm::SmemBmConfig* config);
    static void Uninit(uint32_t flags);
    static uint32_t GetRankId();
    static uint32_t GetRankIdByGva(smem_bm::SmemBmHandle handle, void* gva);
    static smem_bm::SmemBmHandle Create2(
        uint32_t id, const smem_bm::SmemBmCreateOption* option);
    static void Destroy(smem_bm::SmemBmHandle handle);
    static int32_t Join(smem_bm::SmemBmHandle handle, uint32_t flags);
    static int32_t Leave(smem_bm::SmemBmHandle handle, uint32_t flags);
    static int32_t ExtendLocalMem(smem_bm::SmemBmHandle handle,
                                  smem_bm::SmemMemType mem_type, uint64_t size);
    static uint64_t GetLocalMemSize(smem_bm::SmemBmHandle handle,
                                    smem_bm::SmemMemType mem_type);
    static void* PtrByMemType(smem_bm::SmemBmHandle handle,
                              smem_bm::SmemMemType mem_type, uint16_t rank);
    static int32_t CopyBatch(smem_bm::SmemBmHandle handle,
                             smem_bm::SmemBatchCopyParams* params,
                             smem_bm::SmemCopyType type, uint32_t flags);

   private:
    using ConfigInitFn = int32_t (*)(smem_bm::SmemBmConfig*);
    using InitFn = int32_t (*)(const char*, uint32_t, uint16_t,
                               const smem_bm::SmemBmConfig*);
    using UninitFn = void (*)(uint32_t);
    using GetRankIdFn = uint32_t (*)();
    using GetRankIdByGvaFn = uint32_t (*)(smem_bm::SmemBmHandle, void*);
    using Create2Fn =
        smem_bm::SmemBmHandle (*)(uint32_t, const smem_bm::SmemBmCreateOption*);
    using DestroyFn = void (*)(smem_bm::SmemBmHandle);
    using JoinFn = int32_t (*)(smem_bm::SmemBmHandle, uint32_t);
    using LeaveFn = int32_t (*)(smem_bm::SmemBmHandle, uint32_t);
    using ExtendLocalMemFn = int32_t (*)(smem_bm::SmemBmHandle,
                                         smem_bm::SmemMemType, uint64_t);
    using GetLocalMemSizeFn = uint64_t (*)(smem_bm::SmemBmHandle,
                                           smem_bm::SmemMemType);
    using PtrByMemTypeFn = void* (*)(smem_bm::SmemBmHandle,
                                     smem_bm::SmemMemType, uint16_t);
    using CopyBatchFn = int32_t (*)(smem_bm::SmemBmHandle,
                                    smem_bm::SmemBatchCopyParams*,
                                    smem_bm::SmemCopyType, uint32_t);

    static bool LoadLibraryLocked();
    static void ResetSymbols();

    static std::mutex mutex_;
    static void* library_;
    static int ref_count_;
    static bool loaded_;

    static ConfigInitFn config_init_;
    static InitFn init_;
    static UninitFn uninit_;
    static GetRankIdFn get_rank_id_;
    static GetRankIdByGvaFn get_rank_id_by_gva_;
    static Create2Fn create2_;
    static DestroyFn destroy_;
    static JoinFn join_;
    static LeaveFn leave_;
    static ExtendLocalMemFn extend_local_mem_;
    static GetLocalMemSizeFn get_local_mem_size_;
    static PtrByMemTypeFn ptr_by_mem_type_;
    static CopyBatchFn copy_batch_;
};

}  // namespace tent
}  // namespace mooncake

#endif  // TENT_MEMORY_ENGINE_SMEM_BM_API_H
