/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#include "transport/ascend_transport/memfabric_transport/memfabric_transport.h"

#include <acl/acl.h>

#include "transport/ascend_transport/memfabric_transport/memfabric_api.h"

namespace mooncake {
MemFabricTransport::MemFabricTransport() {}

MemFabricTransport::~MemFabricTransport() {}

Status MemFabricTransport::batchCopySmemTrans(const std::unordered_map<SegmentID, std::vector<Slice *>>  &slice_list) {

    for (const auto &items: slice_list) {
        auto segmentId = items.first;
        auto slices = items.second;
        size_t count = slices.size();
        if (count == 0) {
            continue;
        }
        std::string targetName;
        auto targetSegmentDesc = metadata_->getSegmentDescByID(segmentId);
        if (targetSegmentDesc != nullptr) {
            targetName = targetSegmentDesc->name;
        } else {
            LOG(ERROR) << "MemFabricTransport: failed to get segment by id" << segmentId;
        }
        std::vector<const void *> localAddrs(count);
        std::vector<void *> remoteAddrs(count);
        std::vector<uint64_t> dataSizes(count);
        for (size_t i = 0; i < count; ++i) {
            localAddrs[i] = slices[i]->source_addr;
            remoteAddrs[i] = (void *)slices[i]->hccl.dest_addr;
            dataSizes[i] = slices[i]->length;
        }
        auto ret = slices[0]->opcode == TransferRequest::READ ?
            MemFabricSmemDl::SmemTransBatchRead(MemFabricSmemDl::GetSmemTransHandle(),
                                                localAddrs.data(), targetName.c_str(),
                                                remoteAddrs.data(), dataSizes.data(), count) :
            MemFabricSmemDl::SmemTransBatchWrite(MemFabricSmemDl::GetSmemTransHandle(),
                                                 localAddrs.data(), targetName.c_str(),
                                                 remoteAddrs.data(), dataSizes.data(), count);
        if (ret != 0) {
            LOG(ERROR) << "MemFabricTransport: Failed to smem trans copy batch, ret:" << ret;
            for (auto &slice : slices) {
                slice->markFailed();
            }
        } else {
            for (auto &slice : slices) {
                slice->markSuccess();
            }
        }
    }
    return Status::OK();
}

Status MemFabricTransport::batchCopySmemBm(const std::unordered_map<SegmentID, std::vector<Slice *>>  &slice_list) {
    for (const auto &items: slice_list) {
        auto slices = items.second;
        size_t count = slices.size();
        if (count == 0) {
            continue;
        }
        auto opcode = slices[0]->opcode;
        std::vector<const void *> sources(count);
        std::vector<void *> destinations(count);
        std::vector<uint64_t> dataSizes(count);
        for (size_t i = 0; i < count; ++i) {
            sources[i] = (opcode == TransferRequest::READ ? (void *)slices[i]->hccl.dest_addr
                                                          : slices[i]->source_addr);
            destinations[i] = (opcode  == TransferRequest::WRITE ? (void *)slices[i]->hccl.dest_addr
                                                                 : slices[i]->source_addr);
            dataSizes[i] = slices[i]->length;
        }
        smem_batch_copy_params params = {const_cast<void **>(sources.data()), destinations.data(),
                                         dataSizes.data(), static_cast<uint32_t>(count)};

        smem_bm_copy_type t = slices[0]->opcode == TransferRequest::READ ? SMEMB_COPY_G2L : SMEMB_COPY_L2G;
        if (MemFabricSmemDl::GetMemFabricConfig().useLocalHostMemory) {
            t = slices[0]->opcode == TransferRequest::READ ? SMEMB_COPY_G2H : SMEMB_COPY_H2G;
        }
        auto ret = MemFabricSmemDl::SmemBmCopyBatch(MemFabricSmemDl::GetSmemBmHandle(), &params, t, 0);
        if (ret != 0) {
            LOG(ERROR) << "MemFabricTransport: Failed to smem bm copy batch, ret:" << ret;
            for (auto &slice : slices) {
                slice->markFailed();
            }
        } else {
            for (auto &slice : slices) {
                slice->markSuccess();
            }
        }
    }
    return Status::OK();
}

Status MemFabricTransport::batchCopyDefault(const std::unordered_map<SegmentID, std::vector<Slice *>>  &slice_list) {
    for (const auto &items: slice_list) {
        auto slices = items.second;
        for (auto &slice : slices) {
            slice->markFailed();
        }
    }
    return Status::OK();
}

Status MemFabricTransport::submitTransfer(
    Transport::BatchID batch_id, const std::vector<TransferRequest> &entries) {
    auto &batch_desc = *((BatchDesc *)(batch_id));
    if (batch_desc.task_list.size() + entries.size() > batch_desc.batch_size) {
        LOG(ERROR) << "MemFabricTransport: Exceed the limitation of current "
                      "batch's capacity";
        return Status::InvalidArgument(
                "MemFabricTransport: Exceed the limitation of capacity, batch "
                "id: " +
                std::to_string(batch_id));
    }

    auto cur_task_size = batch_desc.task_list.size();
    batch_desc.task_list.resize(cur_task_size + entries.size());
    std::unordered_map<SegmentID, std::vector<Slice *>> slice_list;

    for (auto &request : entries) {
        TransferTask &task = batch_desc.task_list[cur_task_size];
        ++cur_task_size;
        task.total_bytes = request.length;
        Slice *slice = getSliceCache().allocate();
        slice->source_addr = request.source;
        slice->length = request.length;
        slice->opcode = request.opcode;
        slice->target_id = request.target_id;
        slice->hccl.dest_addr = request.target_offset;
        slice->task = &task;
        slice->status = Slice::PENDING;
        task.slice_list.push_back(slice);
        __sync_fetch_and_add(&task.slice_count, 1);
        slice_list[request.target_id].push_back(slice);
    }
    switch (smemType_) {
        case SMEM_BM:
            return batchCopySmemBm(slice_list);
        case SMEM_TRANS:
            return batchCopySmemTrans(slice_list);
        default:
            LOG(ERROR) << "unexpect smem type:" << smemType_;
            return batchCopyDefault(slice_list);
    }
}

Status MemFabricTransport::submitTransferTask(
    const std::vector<TransferTask *> &task_list) {
    std::unordered_map<SegmentID, std::vector<Slice *>> slice_list;
    for (auto index : task_list) {
        auto &task = *index;
        auto &request = *task.request;
        task.total_bytes = request.length;
        Slice *slice = getSliceCache().allocate();
        slice->source_addr = (char *)request.source;
        slice->length = request.length;
        slice->opcode = request.opcode;
        slice->target_id = request.target_id;
        slice->hccl.dest_addr = request.target_offset;
        slice->task = &task;
        slice->status = Slice::PENDING;
        slice->ts = 0;
        task.slice_list.push_back(slice);
        __sync_fetch_and_add(&task.slice_count, 1);
        slice_list[request.target_id].push_back(slice);
    }
    switch (smemType_) {
        case SMEM_BM:
            return batchCopySmemBm(slice_list);
        case SMEM_TRANS:
            return batchCopySmemTrans(slice_list);
        default:
            LOG(ERROR) << "unexpect smem type:" << smemType_;
            return batchCopyDefault(slice_list);
    }
}

Status MemFabricTransport::getTransferStatus(
    Transport::BatchID batch_id, size_t task_id,
    Transport::TransferStatus &status) {
    auto &batch_desc = *((BatchDesc *)(batch_id));
    const size_t task_count = batch_desc.task_list.size();
    if (task_id >= task_count) {
        return Status::InvalidArgument(
            "MemFabricTransport::getTransportStatus invalid argument, batch "
            "id: " +
            std::to_string(batch_id));
    }
    auto &task = batch_desc.task_list[task_id];
    status.transferred_bytes = task.transferred_bytes;
    uint64_t success_slice_count = task.success_slice_count;
    uint64_t failed_slice_count = task.failed_slice_count;
    if (success_slice_count + failed_slice_count == task.slice_count) {
        if (failed_slice_count) {
            status.s = TransferStatusEnum::FAILED;
        } else {
            status.s = TransferStatusEnum::COMPLETED;
        }
        task.is_finished = true;
    } else {
        status.s = TransferStatusEnum::WAITING;
    }
    return Status::OK();
}

int MemFabricTransport::install(std::string &local_server_name,
                                std::shared_ptr<TransferMetadata> meta,
                                std::shared_ptr<Topology> topo) {
    metadata_ = meta;
    local_server_name_ = local_server_name;
    smemType_ = MemFabricSmemDl::GetSmemTypeFlag();

    int ret = aclrtGetDevice(&localDeviceId_);
    if (ret != 0) {
        LOG(ERROR) << "MemFabricTransport: aclrtGetDevice failed, ret: " << ret;
        return ret;
    }
    switch (smemType_) {
        case SMEM_BM:
            ret = MemFabricInitSmemBm(localDeviceId_);
            break;
        case SMEM_TRANS:
            ret = MemFabricInitSmemTrans(local_server_name_, localDeviceId_);
            break;
        default:
            LOG(ERROR) << "unexpect smem type:" << smemType_;
            return -1;
    }
    if (ret != 0) {
        LOG(ERROR) << "MemFabricTransport: failed to init memfabric smem type:" << smemType_ << " ret:" << ret;
        return -1;
    }
    auto desc = std::make_shared<SegmentDesc>();
    if (!desc) return ERR_MEMORY;
    desc->name = local_server_name_;
    desc->protocol = "ascend";
    metadata_->addLocalSegment(LOCAL_SEGMENT_ID, local_server_name_, std::move(desc));
    LOG(INFO) << "MemFabricTransport: add segment type:" << smemType_ << " deviceId:"
              << localDeviceId_ << " segment name:" << local_server_name_ << " protocol:" << desc->protocol;
    return 0;
}

int MemFabricTransport::registerLocalMemory(void *addr, size_t length, const std::string &location,
                                            bool remote_accessible, bool update_metadata)
{
    switch (smemType_) {
        case SMEM_BM:
            return MemFabricSmemDl::SmemBmRegisterUserMem(MemFabricSmemDl::GetSmemBmHandle(),
                                                          reinterpret_cast<uint64_t>(addr), length);
        case SMEM_TRANS:
            return MemFabricSmemDl::SmemTransRegisterMem(MemFabricSmemDl::GetSmemTransHandle(), addr, length);
        default:
            LOG(ERROR) << "unexpect smem type:" << smemType_;
            return -1;
    }
}

int MemFabricTransport::unregisterLocalMemory(void *addr, bool update_metadata)
{
    return 0;
}

int MemFabricTransport::registerLocalMemoryBatch(const std::vector<Transport::BufferEntry> &buffer_list,
                                                 const std::string &location)
{
    switch (smemType_) {
        case SMEM_BM:
            return batchRegisterSmemBmMem(buffer_list, location);
        case SMEM_TRANS:
            return batchRegisterSmemTransMem(buffer_list, location);
        default:
            LOG(ERROR) << "unexpect smem type:" << smemType_;
            return -1;
    }
}

int MemFabricTransport::unregisterLocalMemoryBatch(const std::vector<void *> &addr_list)
{
    return 0;
}

int MemFabricTransport::batchRegisterSmemBmMem(const std::vector<Transport::BufferEntry> &buffer_list,
                                               const std::string &location)
{
    for (const auto &item: buffer_list) {
        auto ret = MemFabricSmemDl::SmemBmRegisterUserMem(MemFabricSmemDl::GetSmemBmHandle(),
                                                          reinterpret_cast<uint64_t>(item.addr),
                                                          item.length);
        if (ret != 0) {
            LOG(ERROR) << "MemFabricTransport: Failed to register user memory ret:" << ret
                       << std::hex << " addr:" << item.addr << " size:" << item.length;
            return ret;
        }
    }
    return 0;
}

int MemFabricTransport::batchRegisterSmemTransMem(const std::vector<Transport::BufferEntry> &buffer_list,
                                                  const std::string &location)
{
    uint32_t count = buffer_list.size();
    std::vector<void *> addrs;
    std::vector<size_t> lengths;
    for (uint32_t i = 0; i < count; ++i) {
        addrs[i] = buffer_list[i].addr;
        lengths[i] = buffer_list[i].length;
    }
    return MemFabricSmemDl::SmemTransBatchRegisterMem(MemFabricSmemDl::GetSmemTransHandle(),
                                                      addrs.data(), lengths.data(), count, 0);
}
}  // namespace mooncake
