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

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "mock_smem_bm.h"
#include "tent/common/config.h"
#include "tent/common/config_lifecycle.h"
#include "tent/memory_engine.h"

namespace mooncake {
namespace tent {
namespace {

constexpr uint64_t kMiB = 1024ULL * 1024ULL;
constexpr uint64_t kAlign = 2ULL * kMiB;
constexpr uint64_t kWindow = 16ULL * kMiB;

uint64_t ByteDiff(const void* later, const void* earlier) {
    return static_cast<uint64_t>(static_cast<const uint8_t*>(later) -
                                 static_cast<const uint8_t*>(earlier));
}

class EnvGuard {
   public:
    EnvGuard(const char* name, const char* value) : name_(name) {
        const char* old = std::getenv(name);
        if (old) {
            old_value_ = old;
            had_value_ = true;
        }
        setenv(name, value, 1);
    }

    ~EnvGuard() {
        if (had_value_) {
            setenv(name_.c_str(), old_value_.c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
    }

   private:
    std::string name_;
    std::string old_value_;
    bool had_value_{false};
};

TEST(MemoryEngineConfigTest, ReadsDefaultsAndOverrides) {
    Config config;
    MemoryEngineConfig engine_config;
    ASSERT_TRUE(MemoryEngineConfig::fromConfig(config, &engine_config).ok());
    EXPECT_EQ(engine_config.world_size, 0u);
    EXPECT_EQ(engine_config.local_rank, 0u);
    EXPECT_EQ(engine_config.host_mem_max, 0u);
    EXPECT_EQ(engine_config.device_mem_max, 0u);
    EXPECT_EQ(engine_config.join_timeout_ms, 120000);
    EXPECT_TRUE(engine_config.enabled);
    EXPECT_TRUE(engine_config.auto_ranking);

    config.set("memory_engine/enabled", false);
    config.set("memory_engine/world_size", 4u);
    config.set("memory_engine/auto_ranking", false);
    config.set("memory_engine/local_rank", 3u);
    config.set("memory_engine/host_mem_max", 1ULL << 30);
    config.set("memory_engine/device_mem_max", 2ULL << 30);
    config.set("memory_engine/join_timeout_ms", 5000);
    ASSERT_TRUE(MemoryEngineConfig::fromConfig(config, &engine_config).ok());
    EXPECT_FALSE(engine_config.enabled);
    EXPECT_EQ(engine_config.world_size, 4u);
    EXPECT_FALSE(engine_config.auto_ranking);
    EXPECT_EQ(engine_config.local_rank, 3u);
    EXPECT_EQ(engine_config.host_mem_max, 1ULL << 30);
    EXPECT_EQ(engine_config.device_mem_max, 2ULL << 30);
    EXPECT_EQ(engine_config.join_timeout_ms, 5000);

    EXPECT_TRUE(
        MemoryEngineConfig::fromConfig(config, nullptr).IsInvalidArgument());
}

TEST(MemoryEngineConfigTest, JoinTimeoutIsRuntimeCandidate) {
    EXPECT_EQ(classifyConfigPath("memory_engine/join_timeout_ms"),
              ConfigLifecycle::kRuntimeCandidate);
    EXPECT_EQ(classifyConfigPath("memory_engine/world_size"),
              ConfigLifecycle::kBootstrapOnly);
}

class MemoryEngineTest : public ::testing::Test {
   protected:
    void SetUp() override {
        setenv("MC_MEMORY_ENGINE_LIB", MOCK_SMEM_BM_LIB, 1);
        setenv("MC_MEMORY_ENGINE_DATA_OP", "device_rdma", 1);
        setenv("MC_MEMORY_ENGINE_DEVICE_ID", "0", 1);
        mock_smem_bm_reset();

        config_ = std::make_shared<Config>();
        config_->set("metadata_type", "etcd");
        config_->set("metadata_servers", "127.0.0.1:2379");

        engine_config_.world_size = 2;
        engine_config_.host_mem_max = kWindow;
        engine_config_.device_mem_max = kWindow;
        engine_config_.join_timeout_ms = 30000;
    }

    void TearDown() override {
        engine_.reset();
        mock_smem_bm_reset();
    }

    Status Create() {
        return MemoryEngine::create(config_, engine_config_, &engine_);
    }

    std::unique_ptr<MemoryEngine> engine_;
    std::shared_ptr<Config> config_;
    MemoryEngineConfig engine_config_;
};

TEST_F(MemoryEngineTest, CreateRejectsInvalidConfig) {
    auto invalid = engine_config_;
    invalid.world_size = 0;
    EXPECT_TRUE(
        MemoryEngine::create(config_, invalid, &engine_).IsInvalidArgument());
    EXPECT_EQ(engine_, nullptr);

    invalid = engine_config_;
    invalid.host_mem_max = 0;
    invalid.device_mem_max = 0;
    EXPECT_TRUE(
        MemoryEngine::create(config_, invalid, &engine_).IsInvalidArgument());

    invalid = engine_config_;
    invalid.auto_ranking = false;
    invalid.local_rank = invalid.world_size;
    EXPECT_TRUE(
        MemoryEngine::create(config_, invalid, &engine_).IsInvalidArgument());

    invalid = engine_config_;
    invalid.enabled = false;
    EXPECT_TRUE(MemoryEngine::create(config_, invalid, &engine_)
                    .IsMemoryEngineDisabled());

    auto no_metadata = std::make_shared<Config>();
    no_metadata->set("metadata_type", "etcd");
    EXPECT_TRUE(MemoryEngine::create(no_metadata, engine_config_, &engine_)
                    .IsInvalidArgument());

    EXPECT_TRUE(MemoryEngine::create(config_, engine_config_, nullptr)
                    .IsInvalidArgument());

    // None of the rejected creates may leave the process-wide backend active.
    ASSERT_TRUE(Create().ok());
    engine_.reset();
}

TEST_F(MemoryEngineTest, QueriesUseTheConfiguredLayout) {
    ASSERT_TRUE(Create().ok()) << "backend creation failed";

    uint32_t rank = 7;
    ASSERT_TRUE(engine_->getLocalRank(&rank).ok());
    EXPECT_EQ(rank, 0u);

    void* host0 = nullptr;
    ASSERT_TRUE(engine_->gvaBase(MemoryEngineMemType::Host, 0, &host0).ok());
    ASSERT_NE(host0, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(host0) % kAlign, 0u);

    void* host1 = nullptr;
    ASSERT_TRUE(engine_->gvaBase(MemoryEngineMemType::Host, 1, &host1).ok());
    EXPECT_EQ(ByteDiff(host1, host0), kWindow);

    void* device0 = nullptr;
    ASSERT_TRUE(
        engine_->gvaBase(MemoryEngineMemType::Device, 0, &device0).ok());
    ASSERT_NE(device0, nullptr);
    EXPECT_NE(device0, host0);

    uint32_t gva_rank = 99;
    ASSERT_TRUE(engine_->rankOfGva(host1, &gva_rank).ok());
    EXPECT_EQ(gva_rank, 1u);
    ASSERT_TRUE(engine_->rankOfGva(device0, &gva_rank).ok());
    EXPECT_EQ(gva_rank, 0u);

    int stack_value = 0;
    EXPECT_TRUE(
        engine_->rankOfGva(&stack_value, &gva_rank).IsInvalidArgument());
    EXPECT_TRUE(engine_->gvaBase(MemoryEngineMemType::Host, 2, &host0)
                    .IsInvalidArgument());
    EXPECT_TRUE(engine_->getLocalRank(nullptr).IsInvalidArgument());
    EXPECT_TRUE(engine_->gvaBase(MemoryEngineMemType::Host, 0, nullptr)
                    .IsInvalidArgument());

    EXPECT_EQ(mock_smem_bm_rank(), 0u);
    EXPECT_EQ(mock_smem_bm_initialized(), 1);
    EXPECT_EQ(mock_smem_bm_joined(), 0);
}

TEST_F(MemoryEngineTest, JoinAllocateFreeAndRejoin) {
    ASSERT_TRUE(Create().ok());

    void* gva = nullptr;
    EXPECT_TRUE(engine_->allocate(kMiB, MemoryEngineMemType::Host, &gva)
                    .IsMemoryEngineNotJoined());

    ASSERT_TRUE(engine_->join().ok());
    EXPECT_EQ(mock_smem_bm_joined(), 1);
    EXPECT_TRUE(engine_->join().IsMemoryEngineAlreadyJoined());

    ASSERT_TRUE(engine_->allocate(kMiB, MemoryEngineMemType::Host, &gva).ok());
    ASSERT_NE(gva, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(gva) % kAlign, 0u);
    EXPECT_EQ(mock_smem_bm_local_size(0), kAlign);
    EXPECT_EQ(mock_smem_bm_extend_calls(0), 1u);

    void* gva2 = nullptr;
    ASSERT_TRUE(
        engine_->allocate(kAlign, MemoryEngineMemType::Host, &gva2).ok());
    EXPECT_EQ(ByteDiff(gva2, gva), kAlign);
    EXPECT_EQ(mock_smem_bm_extend_calls(0), 2u);

    uint32_t gva_rank = 99;
    ASSERT_TRUE(engine_->rankOfGva(gva2, &gva_rank).ok());
    EXPECT_EQ(gva_rank, 0u);

    ASSERT_TRUE(engine_->free(gva).ok());
    EXPECT_TRUE(engine_->free(gva).IsInvalidArgument());

    void* reused = nullptr;
    ASSERT_TRUE(
        engine_->allocate(kAlign, MemoryEngineMemType::Host, &reused).ok());
    EXPECT_EQ(reused, gva);
    EXPECT_EQ(mock_smem_bm_extend_calls(0), 2u);

    void* too_large = reinterpret_cast<void*>(0x1);
    EXPECT_TRUE(
        engine_
            ->allocate(kWindow + kAlign, MemoryEngineMemType::Host, &too_large)
            .IsMemoryEngineNoSpace());
    EXPECT_EQ(too_large, nullptr);

    int dummy = 0;
    void* registered = nullptr;
    EXPECT_TRUE(engine_->registerMemory(&dummy, sizeof(dummy), &registered)
                    .IsMemoryEngineUnsupported());
    EXPECT_TRUE(engine_->unregisterMemory(&dummy).IsMemoryEngineUnsupported());

    ASSERT_TRUE(engine_->leave().ok());
    EXPECT_EQ(mock_smem_bm_joined(), 0);
    EXPECT_TRUE(engine_->leave().IsMemoryEngineNotJoined());
    EXPECT_TRUE(engine_->allocate(kAlign, MemoryEngineMemType::Host, &gva)
                    .IsMemoryEngineNotJoined());
    EXPECT_TRUE(engine_->free(reused).IsMemoryEngineNotJoined());
    std::vector<int32_t> results;
    EXPECT_TRUE(engine_->batchCopy({{&dummy, &dummy, 1}}, &results)
                    .IsMemoryEngineNotJoined());
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0], MEMORY_ENGINE_E_NOT_JOINED);

    ASSERT_TRUE(engine_->join().ok());
    ASSERT_TRUE(
        engine_->allocate(kAlign, MemoryEngineMemType::Host, &gva).ok());
    ASSERT_NE(gva, nullptr);
}

TEST_F(MemoryEngineTest, FreeCoalescesAdjacentBlocks) {
    ASSERT_TRUE(Create().ok());
    ASSERT_TRUE(engine_->join().ok());

    void* a = nullptr;
    void* b = nullptr;
    void* c = nullptr;
    ASSERT_TRUE(engine_->allocate(kAlign, MemoryEngineMemType::Host, &a).ok());
    ASSERT_TRUE(engine_->allocate(kAlign, MemoryEngineMemType::Host, &b).ok());
    ASSERT_TRUE(engine_->allocate(kAlign, MemoryEngineMemType::Host, &c).ok());
    EXPECT_EQ(ByteDiff(b, a), kAlign);
    EXPECT_EQ(ByteDiff(c, b), kAlign);

    ASSERT_TRUE(engine_->free(a).ok());
    ASSERT_TRUE(engine_->free(b).ok());
    const uint64_t extend_calls = mock_smem_bm_extend_calls(0);

    void* merged = nullptr;
    ASSERT_TRUE(
        engine_->allocate(2 * kAlign, MemoryEngineMemType::Host, &merged).ok());
    EXPECT_EQ(merged, a);
    EXPECT_EQ(mock_smem_bm_extend_calls(0), extend_calls);

    ASSERT_TRUE(engine_->free(c).ok());
    ASSERT_TRUE(engine_->free(merged).ok());
    void* fully_merged = nullptr;
    ASSERT_TRUE(
        engine_->allocate(3 * kAlign, MemoryEngineMemType::Host, &fully_merged)
            .ok());
    EXPECT_EQ(fully_merged, a);
    EXPECT_EQ(mock_smem_bm_extend_calls(0), extend_calls);
}

TEST_F(MemoryEngineTest, HostAndDeviceWindowsAreIndependent) {
    ASSERT_TRUE(Create().ok());
    ASSERT_TRUE(engine_->join().ok());

    void* host1 = nullptr;
    void* host2 = nullptr;
    void* device1 = nullptr;
    ASSERT_TRUE(
        engine_->allocate(kAlign, MemoryEngineMemType::Host, &host1).ok());
    ASSERT_TRUE(
        engine_->allocate(kAlign, MemoryEngineMemType::Device, &device1).ok());
    ASSERT_TRUE(
        engine_->allocate(kAlign, MemoryEngineMemType::Host, &host2).ok());

    EXPECT_EQ(ByteDiff(host2, host1), kAlign);
    EXPECT_EQ(mock_smem_bm_local_size(0), 2 * kAlign);
    EXPECT_EQ(mock_smem_bm_local_size(1), kAlign);
    EXPECT_EQ(mock_smem_bm_extend_calls(0), 2u);
    EXPECT_EQ(mock_smem_bm_extend_calls(1), 1u);
    EXPECT_NE(host1, device1);

    void* host_base0 = nullptr;
    void* host_base1 = nullptr;
    void* device_base0 = nullptr;
    void* device_base1 = nullptr;
    ASSERT_TRUE(
        engine_->gvaBase(MemoryEngineMemType::Host, 0, &host_base0).ok());
    ASSERT_TRUE(
        engine_->gvaBase(MemoryEngineMemType::Host, 1, &host_base1).ok());
    ASSERT_TRUE(
        engine_->gvaBase(MemoryEngineMemType::Device, 0, &device_base0).ok());
    ASSERT_TRUE(
        engine_->gvaBase(MemoryEngineMemType::Device, 1, &device_base1).ok());
    EXPECT_EQ(ByteDiff(host_base1, host_base0), kWindow);
    EXPECT_EQ(ByteDiff(device_base1, device_base0), kWindow);
}

TEST_F(MemoryEngineTest, BatchCopyCopiesBothDirectionsAndAttributesFailures) {
    ASSERT_TRUE(Create().ok());
    ASSERT_TRUE(engine_->join().ok());

    void* gva = nullptr;
    ASSERT_TRUE(
        engine_->allocate(kAlign, MemoryEngineMemType::Host, &gva).ok());

    std::vector<uint8_t> source(4096, 0x5a);
    std::vector<uint8_t> destination(4096, 0);
    std::vector<MemoryEngineCopyEntry> entries = {
        {source.data(), gva, source.size()}};
    std::vector<int32_t> results;
    ASSERT_TRUE(engine_->batchCopy(entries, &results).ok());
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0], MEMORY_ENGINE_SUCCESS);
    EXPECT_EQ(std::memcmp(gva, source.data(), source.size()), 0);

    entries = {{gva, destination.data(), destination.size()}};
    ASSERT_TRUE(engine_->batchCopy(entries, &results).ok());
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0], MEMORY_ENGINE_SUCCESS);
    EXPECT_EQ(destination, source);

    std::vector<MemoryEngineCopyEntry> invalid_entries = {
        {nullptr, gva, 16},
        {source.data(), nullptr, 16},
        {source.data(), destination.data(), 0},
    };
    ASSERT_TRUE(engine_->batchCopy(invalid_entries, &results).ok());
    ASSERT_EQ(results.size(), 3u);
    for (int32_t result : results) {
        EXPECT_EQ(result, MEMORY_ENGINE_E_INVALID_ARG);
    }

    void* gva2 = nullptr;
    ASSERT_TRUE(
        engine_->allocate(kAlign, MemoryEngineMemType::Host, &gva2).ok());
    std::vector<uint8_t> destination2(4096, 0);
    std::vector<uint8_t> destination3(4096, 0);
    std::vector<MemoryEngineCopyEntry> mixed_entries = {
        {source.data(), gva2, source.size()},
        {reinterpret_cast<const void*>(kMockSmemBmFailAddress),
         destination2.data(), destination2.size()},
        {gva2, destination3.data(), destination3.size()},
    };
    ASSERT_TRUE(engine_->batchCopy(mixed_entries, &results).ok());
    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0], MEMORY_ENGINE_SUCCESS);
    EXPECT_EQ(results[1], MEMORY_ENGINE_E_INVALID_ARG);
    EXPECT_EQ(results[2], MEMORY_ENGINE_SUCCESS);
    EXPECT_EQ(std::memcmp(gva2, source.data(), source.size()), 0);
    EXPECT_EQ(destination3, source);
    EXPECT_EQ(destination2, std::vector<uint8_t>(4096, 0));

    std::vector<MemoryEngineCopyEntry> empty;
    ASSERT_TRUE(engine_->batchCopy(empty, &results).ok());
    EXPECT_TRUE(results.empty());
    EXPECT_TRUE(engine_->batchCopy(entries, nullptr).IsInvalidArgument());
}

TEST_F(MemoryEngineTest, BackendIsSingletonPerProcess) {
    ASSERT_TRUE(Create().ok());
    std::unique_ptr<MemoryEngine> second;
    EXPECT_TRUE(MemoryEngine::create(config_, engine_config_, &second)
                    .IsInvalidArgument());
    EXPECT_EQ(second, nullptr);
    // The failed second create must not release the first engine's backend.
    ASSERT_TRUE(engine_->join().ok());
    void* gva = nullptr;
    ASSERT_TRUE(
        engine_->allocate(kAlign, MemoryEngineMemType::Host, &gva).ok());

    engine_.reset();
    ASSERT_TRUE(MemoryEngine::create(config_, engine_config_, &second).ok());
    engine_ = std::move(second);
}

TEST_F(MemoryEngineTest, MissingLibraryIsReportedByCreate) {
    EnvGuard guard("MC_MEMORY_ENGINE_LIB",
                   "/nonexistent/libmf_smem_missing.so");
    EXPECT_TRUE(Create().IsDeviceNotFound());
    EXPECT_EQ(engine_, nullptr);
}

TEST_F(MemoryEngineTest, CAbiSmoke) {
    EnvGuard conf_guard(
        "MC_TENT_CONF",
        R"({"metadata_type":"etcd","metadata_servers":"127.0.0.1:2379"})");

    tent_memory_engine_config_t config;
    tent_memory_engine_config_init(&config);
    config.world_size = 1;
    config.host_mem_max = kWindow;
    config.device_mem_max = 0;
    config.join_timeout_ms = 30000;

    tent_memory_engine_t engine = nullptr;
    ASSERT_EQ(tent_memory_engine_create(&config, &engine),
              MEMORY_ENGINE_SUCCESS);
    ASSERT_NE(engine, nullptr);

    EXPECT_EQ(tent_memory_engine_join(engine), MEMORY_ENGINE_SUCCESS);
    EXPECT_EQ(tent_memory_engine_join(engine), MEMORY_ENGINE_E_ALREADY_JOINED);

    uint32_t rank = 99;
    EXPECT_EQ(tent_memory_engine_get_local_rank(engine, &rank),
              MEMORY_ENGINE_SUCCESS);
    EXPECT_EQ(rank, 0u);

    void* base = nullptr;
    EXPECT_EQ(tent_memory_engine_gva_base(engine, TENT_MEMORY_ENGINE_MEM_HOST,
                                          0, &base),
              MEMORY_ENGINE_SUCCESS);
    ASSERT_NE(base, nullptr);

    void* gva = nullptr;
    EXPECT_EQ(tent_memory_engine_allocate(engine, kMiB,
                                          TENT_MEMORY_ENGINE_MEM_HOST, &gva),
              MEMORY_ENGINE_SUCCESS);
    ASSERT_NE(gva, nullptr);

    uint32_t gva_rank = 99;
    EXPECT_EQ(tent_memory_engine_rank_of_gva(engine, gva, &gva_rank),
              MEMORY_ENGINE_SUCCESS);
    EXPECT_EQ(gva_rank, 0u);

    std::memset(base, 0x3c, 16);
    tent_memory_engine_copy_entry_t entry{base, gva, 16};
    int32_t result = -1;
    EXPECT_EQ(tent_memory_engine_batch_copy(engine, &entry, 1, &result),
              MEMORY_ENGINE_SUCCESS);
    EXPECT_EQ(result, MEMORY_ENGINE_SUCCESS);

    void* registered = nullptr;
    EXPECT_EQ(tent_memory_engine_register_memory(engine, base, 16, &registered),
              MEMORY_ENGINE_E_UNSUPPORTED);
    EXPECT_EQ(tent_memory_engine_allocate(engine, kMiB,
                                          TENT_MEMORY_ENGINE_MEM_DEVICE, &gva),
              MEMORY_ENGINE_E_NO_SPACE);

    EXPECT_EQ(tent_memory_engine_leave(engine), MEMORY_ENGINE_SUCCESS);
    EXPECT_EQ(tent_memory_engine_allocate(engine, kMiB,
                                          TENT_MEMORY_ENGINE_MEM_HOST, &gva),
              MEMORY_ENGINE_E_NOT_JOINED);
    EXPECT_EQ(tent_memory_engine_allocate(engine, kMiB,
                                          TENT_MEMORY_ENGINE_MEM_DEVICE, &gva),
              MEMORY_ENGINE_E_NOT_JOINED);

    tent_memory_engine_destroy(engine);

    EXPECT_EQ(tent_memory_engine_create(&config, nullptr),
              MEMORY_ENGINE_E_INVALID_ARG);
    EXPECT_EQ(tent_memory_engine_gva_base(nullptr, 0, 0, &base),
              MEMORY_ENGINE_E_INVALID_ARG);
    EXPECT_EQ(tent_memory_engine_allocate(nullptr, kMiB, 99, &gva),
              MEMORY_ENGINE_E_INVALID_ARG);
}

}  // namespace
}  // namespace tent
}  // namespace mooncake
