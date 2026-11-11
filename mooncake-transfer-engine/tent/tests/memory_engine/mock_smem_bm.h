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

#ifndef TENT_TESTS_MOCK_SMEM_BM_H
#define TENT_TESTS_MOCK_SMEM_BM_H

#include <cstdint>

// Any copy entry that uses this address fails in the mock library, which lets
// tests exercise batchCopy's per-entry failure attribution.
constexpr uintptr_t kMockSmemBmFailAddress = 0xdeadbeefULL;

extern "C" {

void mock_smem_bm_reset();

// Observability helpers used by the unit tests.
uint64_t mock_smem_bm_local_size(int mem_type);
uint64_t mock_smem_bm_extend_calls(int mem_type);
uint32_t mock_smem_bm_rank();
int mock_smem_bm_joined();
int mock_smem_bm_initialized();

}  // extern "C"

#endif  // TENT_TESTS_MOCK_SMEM_BM_H
