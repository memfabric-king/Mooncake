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

#include "smem_bm_api.h"

#include <dlfcn.h>

#include <cstdlib>

#include <glog/logging.h>

namespace mooncake {
namespace tent {
namespace {

const char* MemoryEngineLibraryPath() {
    const char* path = std::getenv("MC_MEMORY_ENGINE_LIB");
    return (path && *path) ? path : "libmf_smem.so";
}

}  // namespace

std::mutex SmemBmUnderApi::mutex_;
void* SmemBmUnderApi::library_ = nullptr;
int SmemBmUnderApi::ref_count_ = 0;
bool SmemBmUnderApi::loaded_ = false;

SmemBmUnderApi::ConfigInitFn SmemBmUnderApi::config_init_ = nullptr;
SmemBmUnderApi::InitFn SmemBmUnderApi::init_ = nullptr;
SmemBmUnderApi::UninitFn SmemBmUnderApi::uninit_ = nullptr;
SmemBmUnderApi::GetRankIdFn SmemBmUnderApi::get_rank_id_ = nullptr;
SmemBmUnderApi::Create2Fn SmemBmUnderApi::create2_ = nullptr;
SmemBmUnderApi::DestroyFn SmemBmUnderApi::destroy_ = nullptr;
SmemBmUnderApi::JoinFn SmemBmUnderApi::join_ = nullptr;
SmemBmUnderApi::LeaveFn SmemBmUnderApi::leave_ = nullptr;
SmemBmUnderApi::ExtendLocalMemFn SmemBmUnderApi::extend_local_mem_ = nullptr;
SmemBmUnderApi::GetLocalMemSizeFn SmemBmUnderApi::get_local_mem_size_ = nullptr;
SmemBmUnderApi::PtrByMemTypeFn SmemBmUnderApi::ptr_by_mem_type_ = nullptr;
SmemBmUnderApi::CopyBatchFn SmemBmUnderApi::copy_batch_ = nullptr;

#define TENT_SMEM_BM_LOAD_SYM(field, name)                                 \
    do {                                                                   \
        field = reinterpret_cast<decltype(field)>(dlsym(library_, name));  \
        if (!field) {                                                      \
            LOG(ERROR) << "Failed to dlsym " << name << ": " << dlerror(); \
            ok = false;                                                    \
        }                                                                  \
    } while (0)

bool SmemBmUnderApi::LoadLibrary() {
    std::lock_guard<std::mutex> lock(mutex_);
    return LoadLibraryLocked();
}

bool SmemBmUnderApi::LoadLibraryLocked() {
    if (loaded_) {
        ++ref_count_;
        return true;
    }

    const char* path = MemoryEngineLibraryPath();
    library_ = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!library_) {
        LOG(ERROR) << "Failed to dlopen " << path << ": " << dlerror();
        return false;
    }

    bool ok = true;
    TENT_SMEM_BM_LOAD_SYM(config_init_, "smem_bm_config_init");
    TENT_SMEM_BM_LOAD_SYM(init_, "smem_bm_init");
    TENT_SMEM_BM_LOAD_SYM(uninit_, "smem_bm_uninit");
    TENT_SMEM_BM_LOAD_SYM(get_rank_id_, "smem_bm_get_rank_id");
    TENT_SMEM_BM_LOAD_SYM(create2_, "smem_bm_create2");
    TENT_SMEM_BM_LOAD_SYM(destroy_, "smem_bm_destroy");
    TENT_SMEM_BM_LOAD_SYM(join_, "smem_bm_join");
    TENT_SMEM_BM_LOAD_SYM(leave_, "smem_bm_leave");
    TENT_SMEM_BM_LOAD_SYM(extend_local_mem_, "smem_bm_extend_local_mem");
    TENT_SMEM_BM_LOAD_SYM(get_local_mem_size_,
                          "smem_bm_get_local_mem_size_by_mem_type");
    TENT_SMEM_BM_LOAD_SYM(ptr_by_mem_type_, "smem_bm_ptr_by_mem_type");
    TENT_SMEM_BM_LOAD_SYM(copy_batch_, "smem_bm_copy_batch");

    if (!ok) {
        dlclose(library_);
        library_ = nullptr;
        ResetSymbols();
        return false;
    }

    loaded_ = true;
    ref_count_ = 1;
    return true;
}

void SmemBmUnderApi::UnloadLibrary() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!loaded_) return;
    if (--ref_count_ > 0) return;
    if (library_) dlclose(library_);
    library_ = nullptr;
    ResetSymbols();
    loaded_ = false;
}

bool SmemBmUnderApi::IsLoaded() {
    std::lock_guard<std::mutex> lock(mutex_);
    return loaded_;
}

void SmemBmUnderApi::ResetSymbols() {
    config_init_ = nullptr;
    init_ = nullptr;
    uninit_ = nullptr;
    get_rank_id_ = nullptr;
    create2_ = nullptr;
    destroy_ = nullptr;
    join_ = nullptr;
    leave_ = nullptr;
    extend_local_mem_ = nullptr;
    get_local_mem_size_ = nullptr;
    ptr_by_mem_type_ = nullptr;
    copy_batch_ = nullptr;
}

int32_t SmemBmUnderApi::ConfigInit(smem_bm::SmemBmConfig* config) {
    return config_init_ ? config_init_(config) : smem_bm::kError;
}

int32_t SmemBmUnderApi::Init(const char* store_url, uint32_t world_size,
                             uint16_t device_id,
                             const smem_bm::SmemBmConfig* config) {
    return init_ ? init_(store_url, world_size, device_id, config)
                 : smem_bm::kError;
}

void SmemBmUnderApi::Uninit(uint32_t flags) {
    if (uninit_) uninit_(flags);
}

uint32_t SmemBmUnderApi::GetRankId() {
    return get_rank_id_ ? get_rank_id_() : UINT32_MAX;
}

smem_bm::SmemBmHandle SmemBmUnderApi::Create2(
    uint32_t id, const smem_bm::SmemBmCreateOption* option) {
    return create2_ ? create2_(id, option) : nullptr;
}

void SmemBmUnderApi::Destroy(smem_bm::SmemBmHandle handle) {
    if (destroy_) destroy_(handle);
}

int32_t SmemBmUnderApi::Join(smem_bm::SmemBmHandle handle, uint32_t flags) {
    return join_ ? join_(handle, flags) : smem_bm::kError;
}

int32_t SmemBmUnderApi::Leave(smem_bm::SmemBmHandle handle, uint32_t flags) {
    return leave_ ? leave_(handle, flags) : smem_bm::kError;
}

int32_t SmemBmUnderApi::ExtendLocalMem(smem_bm::SmemBmHandle handle,
                                       smem_bm::SmemMemType mem_type,
                                       uint64_t size) {
    return extend_local_mem_ ? extend_local_mem_(handle, mem_type, size)
                             : smem_bm::kError;
}

uint64_t SmemBmUnderApi::GetLocalMemSize(smem_bm::SmemBmHandle handle,
                                         smem_bm::SmemMemType mem_type) {
    return get_local_mem_size_ ? get_local_mem_size_(handle, mem_type) : 0;
}

void* SmemBmUnderApi::PtrByMemType(smem_bm::SmemBmHandle handle,
                                   smem_bm::SmemMemType mem_type,
                                   uint16_t rank) {
    return ptr_by_mem_type_ ? ptr_by_mem_type_(handle, mem_type, rank)
                            : nullptr;
}

int32_t SmemBmUnderApi::CopyBatch(smem_bm::SmemBmHandle handle,
                                  smem_bm::SmemBatchCopyParams* params,
                                  smem_bm::SmemCopyType type, uint32_t flags) {
    return copy_batch_ ? copy_batch_(handle, params, type, flags)
                       : smem_bm::kError;
}

}  // namespace tent
}  // namespace mooncake
