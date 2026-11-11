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

#ifndef TENT_MEMORY_ENGINE_SMEM_BM_ABI_H
#define TENT_MEMORY_ENGINE_SMEM_BM_ABI_H

// A minimal, ABI-compatible mirror of the MemFabric smem_bm C interface.
// MemoryEngine loads libmf_smem.so at runtime, so it cannot include the vendor
// headers at build time. Keep the layout in sync with:
//   memfabric_hybrid/src/smem/include/host/smem_bm_def.h

#include <cstddef>
#include <cstdint>

namespace mooncake {
namespace tent {
namespace smem_bm {

constexpr uint32_t kTlsPathSize = 256;
constexpr uint64_t kLargePageSize = 2ULL * 1024 * 1024;
constexpr uint64_t kMaxLocalDramSize = 2ULL << 40;  // 2 TiB
constexpr uint64_t kMaxLocalHbmSize = 64ULL << 30;  // 64 GiB

// SMErrorCode values.
constexpr int32_t kOk = 0;
constexpr int32_t kError = -1;
constexpr int32_t kInvalidParam = -2000;
constexpr int32_t kTimeout = -2004;
constexpr int32_t kNotInitialized = -2008;
constexpr int32_t kPartialFailed = -2012;
constexpr int32_t kNeedRetry = -2015;

enum SmemMemType : int32_t {
    kLocalDevice = 0,
    kLocalHost = 1,
    kGlobalDevice = 2,
    kGlobalHost = 3,
};

enum SmemDataOpType : uint32_t {
    kDataOpSdma = 1U << 0,
    kDataOpHostRdma = 1U << 1,
    kDataOpHostTcp = 1U << 2,
    kDataOpDeviceRdma = 1U << 3,
    kDataOpHostUrma = 1U << 4,
    kDataOpHostShm = 1U << 5,
    kDataOpDeviceUrma = 1U << 6,
    kDataOpDeviceUboe = 1U << 7,
};

enum SmemCopyType : int32_t {
    kCopyL2G = 0,
    kCopyG2L = 1,
    kCopyG2H = 2,
    kCopyH2G = 3,
    kCopyL2GH = 4,
    kCopyGH2L = 5,
    kCopyGH2H = 6,
    kCopyH2GH = 7,
    kCopyG2G = 8,
    kCopyAuto = 9,
};

struct SmemTlsConfig {
    bool tlsEnable;
    char caPath[kTlsPathSize];
    char crlPath[kTlsPathSize];
    char certPath[kTlsPathSize];
    char keyPath[kTlsPathSize];
    char keyPassPath[kTlsPathSize];
    char packagePath[kTlsPathSize];
    char decrypterLibPath[kTlsPathSize];
};

struct SmemBmConfig {
    uint32_t initTimeout;
    uint32_t createTimeout;
    uint32_t controlOperationTimeout;
    bool startConfigStoreServer;
    bool startConfigStoreOnly;
    bool dynamicWorldSize;
    bool unifiedAddressSpace;
    bool autoRanking;
    uint32_t rankId;
    uint32_t flags;
    char hcomUrl[64];
    SmemTlsConfig hcomTlsConfig;
    SmemTlsConfig storeTlsConfig;
};

struct SmemBmCreateOption {
    uint64_t maxDramSize;
    uint64_t maxHbmSize;
    uint64_t localDRAMSize;
    uint64_t localHBMSize;
    SmemDataOpType dataOpType;
    bool enable56BitsGva;
    uint32_t flags;
    char tag[32];
    char tagOpInfo[256];
    int32_t dramShmFd;
};

struct SmemBatchCopyParams {
    void** sources;
    void** destinations;
    const uint64_t* dataSizes;
    uint32_t batchSize;
    void* stream;
};

using SmemBmHandle = void*;

}  // namespace smem_bm
}  // namespace tent
}  // namespace mooncake

#endif  // TENT_MEMORY_ENGINE_SMEM_BM_ABI_H
