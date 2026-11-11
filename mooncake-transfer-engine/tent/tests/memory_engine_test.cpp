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
#include <memory>
#include <string>

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
    EXPECT_EQ(engine_config.join_timeout_s, 120u);
    EXPECT_TRUE(engine_config.enabled_server);
    EXPECT_TRUE(engine_config.auto_ranking);
    EXPECT_TRUE(engine_config.store_url.empty());

    config.set("metadata_type", "etcd");
    config.set("metadata_servers", "127.0.0.1:2379");
    ASSERT_TRUE(MemoryEngineConfig::fromConfig(config, &engine_config).ok());
    EXPECT_EQ(engine_config.store_url, "etcd://127.0.0.1:2379");

    config.set("memory_engine/store_url", "tcp://127.0.0.1:9999");
    ASSERT_TRUE(MemoryEngineConfig::fromConfig(config, &engine_config).ok());
    EXPECT_EQ(engine_config.store_url, "tcp://127.0.0.1:9999");

    config.set("memory_engine/enabled_server", false);
    config.set("memory_engine/world_size", 4u);
    config.set("memory_engine/auto_ranking", false);
    config.set("memory_engine/local_rank", 3u);
    config.set("memory_engine/host_mem_max", 1ULL << 30);
    config.set("memory_engine/device_mem_max", 2ULL << 30);
    config.set("memory_engine/join_timeout_s", 5u);
    ASSERT_TRUE(MemoryEngineConfig::fromConfig(config, &engine_config).ok());
    EXPECT_FALSE(engine_config.enabled_server);
    EXPECT_EQ(engine_config.world_size, 4u);
    EXPECT_FALSE(engine_config.auto_ranking);
    EXPECT_EQ(engine_config.local_rank, 3u);
    EXPECT_EQ(engine_config.host_mem_max, 1ULL << 30);
    EXPECT_EQ(engine_config.device_mem_max, 2ULL << 30);
    EXPECT_EQ(engine_config.join_timeout_s, 5u);

    EXPECT_TRUE(
        MemoryEngineConfig::fromConfig(config, nullptr).IsInvalidArgument());
}

TEST(MemoryEngineConfigTest, JoinTimeoutIsRuntimeCandidate) {
    EXPECT_EQ(classifyConfigPath("memory_engine/join_timeout_s"),
              ConfigLifecycle::kRuntimeCandidate);
    EXPECT_EQ(classifyConfigPath("memory_engine/world_size"),
              ConfigLifecycle::kBootstrapOnly);
}

class MemoryEngineTest : public ::testing::Test {
   protected:
    void SetUp() override {
        setenv("MC_MEMORY_ENGINE_LIB", MOCK_SMEM_BM_LIB, 1);
        setenv("MC_MEMORY_ENGINE_DATA_OP", "device_rdma", 1);
        mock_smem_bm_reset();

        engine_config_.world_size = 2;
        engine_config_.host_mem_max = kWindow;
        engine_config_.device_mem_max = kWindow;
        engine_config_.join_timeout_s = 30;
        engine_config_.store_url = "etcd://127.0.0.1:2379";
    }

    void TearDown() override {
        engine_.reset();
        mock_smem_bm_reset();
    }

    Status Create() { return MemoryEngine::create(engine_config_, &engine_); }

    std::unique_ptr<MemoryEngine> engine_;
    MemoryEngineConfig engine_config_;
};

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

TEST_F(MemoryEngineTest, JoinLeaveRejoin) {
    ASSERT_TRUE(Create().ok());

    uint32_t rank = 99;
    ASSERT_TRUE(engine_->getLocalRank(&rank).ok());
    EXPECT_EQ(rank, 0u);

    ASSERT_TRUE(engine_->join().ok());
    EXPECT_EQ(mock_smem_bm_joined(), 1);
    EXPECT_TRUE(engine_->join().IsMemoryEngineAlreadyJoined());

    ASSERT_TRUE(engine_->leave().ok());
    EXPECT_EQ(mock_smem_bm_joined(), 0);
    EXPECT_TRUE(engine_->leave().IsMemoryEngineNotJoined());
    EXPECT_TRUE(engine_->getLocalRank(&rank).IsMemoryEngineNotJoined());

    ASSERT_TRUE(engine_->join().ok());
    ASSERT_TRUE(engine_->getLocalRank(&rank).ok());
    EXPECT_EQ(rank, 0u);
}

TEST_F(MemoryEngineTest, MissingLibraryIsReportedByCreate) {
    EnvGuard guard("MC_MEMORY_ENGINE_LIB",
                   "/nonexistent/libmf_smem_missing.so");
    EXPECT_TRUE(Create().IsDeviceNotFound());
    EXPECT_EQ(engine_, nullptr);
}

}  // namespace
}  // namespace tent
}  // namespace mooncake
