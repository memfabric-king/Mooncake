#include <algorithm>
#include "transport/ascend_transport/memfabric_transport/memfabric_api.h"

namespace mooncake {

bool MemFabricSmemDl::gLoaded_ = false;
std::mutex MemFabricSmemDl::mutex_;
smem_bm_t MemFabricSmemDl::smemBmHandle_ = nullptr;
smem_trans_t MemFabricSmemDl::smemTransHandle_ = nullptr;
MemFabricConfig MemFabricSmemDl::config_;
smem_type MemFabricSmemDl::smemTypeFlag_ = SMEM_TRANS; // default trans
FUNC_SMEM_BM_CONFIG_INIT MemFabricSmemDl::pSmemBmConfigInit = nullptr;
FUNC_SMEM_BM_INIT MemFabricSmemDl::pSmemBmInit = nullptr;
FUNC_SMEM_BM_UNINIT MemFabricSmemDl::pSmemBmUninit = nullptr;
FUNC_SMEM_BM_GET_RANK_ID MemFabricSmemDl::pSmemGetRankId = nullptr;
FUNC_SMEM_BM_CREATE MemFabricSmemDl::pSmemBmCreate = nullptr;
FUNC_SMEM_BM_DESTROY MemFabricSmemDl::pSmemBmDestory = nullptr;
FUNC_SMEM_BM_JOIN MemFabricSmemDl::pSmemBmJoin = nullptr;
FUNC_SMEM_BM_LEAVE MemFabricSmemDl::pSmemBmLeave = nullptr;
FUNC_SMEM_BM_GET_LOCAL_MEM_SIZE_BY_MEM_TYPE
MemFabricSmemDl::pSmemBmGetLocalMemSizeByMemType = nullptr;
FUNC_SMEM_BM_PTR_BY_MEM_TYPE MemFabricSmemDl::pSmemBmPtrByMemType = nullptr;
FUNC_SMEM_BM_COPY MemFabricSmemDl::pSmemBmCopy = nullptr;
FUNC_SMEM_BM_COPY_BATCH MemFabricSmemDl::pSmemBmCopyBatch = nullptr;
FUNC_SMEM_BM_WAIT MemFabricSmemDl::pSmemBmWait = nullptr;
FUNC_SMEM_BM_REGISTER_USER_MEM MemFabricSmemDl::pSmemBmRegisterUserMem =
    nullptr;
FUNC_SMEM_SET_EXTERN_LOGGER MemFabricSmemDl::pSmemSetExternLogger = nullptr;
FUNC_SMEM_SET_LOG_LEVEL MemFabricSmemDl::pSmemSetLogLevel = nullptr;
FUNC_SMEM_CREATE_CONFIG_STORE MemFabricSmemDl::pSmemCreateConfigStore =
    nullptr;

FUNC_SMEM_TRANS_INIT MemFabricSmemDl::pSmemTransInit = nullptr;
FUNC_SMEM_TRANS_UNINIT MemFabricSmemDl::pSmemTransUnInit = nullptr;
FUNC_SMEM_TRANS_CREATE MemFabricSmemDl::pSmemTransCreate = nullptr;
FUNC_SMEM_TRANS_DESTORY MemFabricSmemDl::pSmemTransDestory = nullptr;
FUNC_SMEM_TRANS_REGISTER_MEM MemFabricSmemDl::pSmemTransRegisterMem = nullptr;
FUNC_SMEM_TRANS_UNREGISTER_MEM MemFabricSmemDl::pSmemTransUnregisterMem = nullptr;
FUNC_SMEM_TRANS_BATCH_REGISTER_MEM MemFabricSmemDl::pSmemTransBatchRegisterMem = nullptr;
FUNC_SMEM_TRANS_WRITE MemFabricSmemDl::pSmemTransWrite = nullptr;
FUNC_SMEM_TRANS_BATCH_WRITE MemFabricSmemDl::pSmemTransBatchWrite = nullptr;
FUNC_SMEM_TRANS_BATCH_READ MemFabricSmemDl::pSmemTransBatchRead = nullptr;

static void MfRegisterLogger(int level, const char* msg) {
    switch (level) {
        case 0:
            LOG(INFO) << msg;
            break;
        case 1:
            LOG(INFO) << msg;
            break;
        case 2:
            LOG(WARNING) << msg;
            break;
        case 3:
            LOG(ERROR) << msg;
            break;
        default:
            LOG(INFO) << msg;
            break;
    };
}

int InitMemFabricLog() {
    if (MemFabricSmemDl::SmemSetLogLevel(
            MemFabricSmemDl::GetMemFabricConfig().logLevel) != 0) {
        LOG(ERROR) << "mf bm set logger level failed";
        return -1;
    }

    if (MemFabricSmemDl::SmemSetExternLogger(MfRegisterLogger) != 0) {
        LOG(ERROR) << "mf bm set logger func failed";
        return -1;
    }
    return 0;
}

int InitMemFabricBm(int deviceId) {
    std::string storeUrl;
    std::string connUrl;
    smem_bm_config_t config = {};
    if (MemFabricSmemDl::SmemBmConfigInit(&config) != 0) {
        LOG(ERROR) << "Failed to init smem bm config";
        return -1;
    }
    std::string storeUrlStr = MemFabricSmemDl::GetMemFabricConfig().storeUrl;
    size_t colon_pos = storeUrlStr.find(':');
    if (colon_pos == std::string::npos) {
        LOG(ERROR) << "Unexpected master server addr: " << storeUrlStr;
        return -1;
    } else {
        std::string ip = storeUrlStr.substr(0, colon_pos);
        std::string portStr = storeUrlStr.substr(colon_pos + 1);
        int port = -1;
        try {
            port = std::stoi(portStr);
            port += 1;
            if (port < 1024 || port > 65534) {
                LOG(ERROR) << "Port out of range:" << port;
                return -1;
            }
        } catch (...) {
            LOG(ERROR) << "Error: Invalid argument, cannot convert '"
                       << portStr << "' to integer.";
            return -1;
        }
        storeUrl = "tcp://" + ip + ":" + std::to_string(port);
        connUrl = "tcp://" + ip + ":" + std::to_string(port + 1);
    }
    config.flags |= SMEM_BM_INIT_GVM_FLAG;
    config.startConfigStoreServer = true;
    std::copy_n(connUrl.c_str(),
                std::min(sizeof(config.hcomUrl) - 1, connUrl.size()),
                config.hcomUrl);
    if (MemFabricSmemDl::SmemBmInit(storeUrl.c_str(), MemFabricSmemDl::GetMemFabricConfig().worldSize,
                                    deviceId, &config) != 0) {
        LOG(ERROR) << "Failed to init smem bm:" << storeUrl;
        return -1;
    }
    return 0;
}

int MemFabricInitSmemBm(int deviceId) {
    if (MemFabricSmemDl::LoadMemFabricBmAPI() != 0) {
        LOG(ERROR) << "Error: Failed to load MemFabric api";
        return -1;
    }
    smem_bm_t handle = MemFabricSmemDl::GetSmemBmHandle();
    if (handle != nullptr) {
        LOG(INFO) << "mf bm already init";
        return 0;
    }
    // log
    auto ret = InitMemFabricLog();
    if (ret != 0) {
        return ret;
    }
    // init
    ret = InitMemFabricBm(deviceId);
    if (ret != 0) {
        return ret;
    }
    // create
    uint64_t localHBMSize = MemFabricSmemDl::GetMemFabricConfig().hbmSize;
    uint64_t localDRAMSize = MemFabricSmemDl::GetMemFabricConfig().dramSize;
    smem_bm_data_op_type opType = MemFabricSmemDl::GetMemFabricConfig().opType;
    handle = MemFabricSmemDl::SmemBmCreate(0, 0, opType, localDRAMSize,
                                           localHBMSize, 0);
    if (handle == nullptr) {
        MemFabricSmemDl::SmemBmUninit(0);
        LOG(ERROR) << "Failed to create smem bm";
        return -1;
    }
    // join
    if (MemFabricSmemDl::SmemBmJoin(handle, 0) != 0) {
        MemFabricSmemDl::SmemBmDestory(handle);
        MemFabricSmemDl::SmemBmUninit(0);
        LOG(ERROR) << "Failed to join smem bm";
        return -1;
    }
    return 0;
}

int InitMemFabricSmemTrans(const std::string &sessionId, int deviceId) {
    std::string storeUrl;
    std::string storeUrlStr = MemFabricSmemDl::GetMemFabricConfig().storeUrl;
    size_t colon_pos = storeUrlStr.find(':');
    if (colon_pos == std::string::npos) {
        LOG(ERROR) << "Unexpected store url: " << storeUrlStr;
        return -1;
    } else {
        std::string ip = storeUrlStr.substr(0, colon_pos);
        std::string portStr = storeUrlStr.substr(colon_pos + 1);
        int port = -1;
        try {
            port = std::stoi(portStr);
            if (port < 1024 || port > 65534) {
                LOG(ERROR) << "Port out of range:" << port;
                return -1;
            }
        } catch (...) {
            LOG(ERROR) << "Error: Invalid argument, cannot convert '"
                       << portStr << "' to integer.";
            return -1;
        }
        storeUrl = "tcp://" + ip + ":" + std::to_string(port);
    }
    smem_trans_config_t config{};
    config.initTimeout = 120;
    config.deviceId = deviceId;
    config.flags |= SMEM_BM_INIT_GVM_FLAG;
    config.role = MemFabricSmemDl::GetMemFabricConfig().role;
    config.dataOpType = MemFabricSmemDl::GetMemFabricConfig().transOpType;
    config.startConfigServer = true;
    if (MemFabricSmemDl::SmemTranInit(&config) != 0) {
        LOG(ERROR) << "Failed to init smem trans:" << storeUrl;
        return -1;
    }
    // create
    if (MemFabricSmemDl::SmemTransCreate(storeUrl.c_str(), sessionId.c_str(), &config) == nullptr) {
        LOG(ERROR) << "Failed to init smem trans:" << storeUrl;
        return -1;
    }
    return 0;
}

int MemFabricInitSmemTrans(const std::string& sessionId, int deviceId) {
    if (MemFabricSmemDl::LoadMemFabricBmAPI() != 0) {
        LOG(ERROR) << "Error: Failed to load MemFabric api";
        return -1;
    }
    smem_trans_t handle = MemFabricSmemDl::GetSmemTransHandle();
    if (handle != nullptr) {
        LOG(INFO) << "mf smem trans already init";
        return 0;
    }
    // log
    auto ret = InitMemFabricLog();
    if (ret != 0) {
        return ret;
    }
    // init and create
    ret = InitMemFabricSmemTrans(sessionId, deviceId);
    if (ret != 0) {
        return ret;
    }
    return 0;
}

int MemFabricInitConfigStore(const std::string& store_address,
                             uint32_t storePort) {
    mooncake::MemFabricSmemDl::LoadMemFabricBmAPI();
    if (storePort < 1024 || storePort > 65535) {
        LOG(FATAL) << "StorePort out of range:" << storePort;
        return -1;
    }
    std::string storeUrl =
        "tcp://" + store_address + ":" + std::to_string(storePort);
    int32_t ret = mooncake::MemFabricSmemDl::SmemSetLogLevel(1);
    if (ret != 0) {
        LOG(FATAL) << "mf set logger level failed:" << ret;
        return -1;
    }
    ret = mooncake::MemFabricSmemDl::SmemSetExternLogger(
        mooncake::MfRegisterLogger);
    if (ret != 0) {
        LOG(FATAL) << "mf set logger func failed:" << ret;
        return -1;
    }
    if (mooncake::MemFabricSmemDl::SmemCreateConfigStore(storeUrl.c_str()) !=
        0) {
        LOG(FATAL) << "mf create config store failed url:" << storeUrl;
        return -1;
    }
    return 0;
}

std::pair<void*, size_t> MemFabricGetSegment() {
    auto bmRankId = MemFabricSmemDl::SmemGetRankId();
    auto handle = MemFabricSmemDl::GetSmemBmHandle();
    void* ptr = MemFabricSmemDl::SmemBmPtrByMemType(
        handle, SMEM_MEM_TYPE_HOST, bmRankId);
    size_t dramSize = MemFabricSmemDl::SmemBmGetLocalMemSizeByMemType(
        handle, SMEM_MEM_TYPE_HOST);
    return {ptr, dramSize};
}

}  // namespace mooncake