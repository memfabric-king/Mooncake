// Copyright 2025 Huawei Technologies Co., Ltd
// Copyright 2024 KVCache.AI
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

#ifndef MEMFBARIC_TRANSPORT_H
#define MEMFBARIC_TRANSPORT_H

#include <infiniband/verbs.h>
#include <atomic>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <condition_variable>
#include "transfer_metadata.h"
#include "transport/transport.h"
#include "transport/ascend_transport/memfabric_transport/memfabric_api.h"

namespace mooncake {
class TransferMetadata;
class MemFabricTransport : public Transport {
public:
    using SegmentDesc = TransferMetadata::SegmentDesc;

public:
    MemFabricTransport();

    ~MemFabricTransport();

    Status submitTransfer(BatchID batch_id,
                          const std::vector<TransferRequest> &entries) override;

    Status submitTransferTask(
        const std::vector<TransferTask *> &task_list) override;

    Status getTransferStatus(BatchID batch_id, size_t task_id,
                             TransferStatus &status) override;

    int install(std::string &local_server_name,
                std::shared_ptr<TransferMetadata> meta,
                std::shared_ptr<Topology> topo) override;

    const char *getName() const override { return "MemFabric"; }

    int registerLocalMemory(void *addr, size_t length,
                            const std::string &location, bool remote_accessible,
                            bool update_metadata) override;

    int unregisterLocalMemory(void *addr,
                              bool update_metadata = false) override;

    int registerLocalMemoryBatch(
        const std::vector<Transport::BufferEntry> &buffer_list,
        const std::string &location) override;

    int unregisterLocalMemoryBatch(
        const std::vector<void *> &addr_list) override;

private:
    int batchRegisterSmemBmMem(const std::vector<Transport::BufferEntry> &buffer_list,
                               const std::string &location);
    int batchRegisterSmemTransMem(const std::vector<Transport::BufferEntry> &buffer_list,
                               const std::string &location);
    Status batchCopySmemBm(const std::unordered_map<SegmentID, std::vector<Slice *>>  &slice_list);
    Status batchCopySmemTrans(const std::unordered_map<SegmentID, std::vector<Slice *>>  &slice_list);
    Status batchCopyDefault(const std::unordered_map<SegmentID, std::vector<Slice *>>  &slice_list);

private:
    smem_type smemType_{SMEM_BUTT};
    int32_t localDeviceId_{-1};
};
}  // namespace mooncake
#endif