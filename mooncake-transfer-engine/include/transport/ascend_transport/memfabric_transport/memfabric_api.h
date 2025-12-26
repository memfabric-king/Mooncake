#pragma once

#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <climits>
#include <unistd.h>
#include <string>
#include <mutex>
#include <dlfcn.h>
#include <sys/stat.h>
#include <glog/logging.h>

namespace mooncake {
typedef enum {
    SMEM_BM,
    SMEM_TRANS,
    SMEM_BUTT
} smem_type;

typedef enum {
    SMEM_MEM_TYPE_LOCAL_DEVICE = 0, /* memory on local device */
    SMEM_MEM_TYPE_LOCAL_HOST,       /* memory on local host */
    SMEM_MEM_TYPE_DEVICE,           /* memory on global device */
    SMEM_MEM_TYPE_HOST,             /* memory on global host */

    SMEM_MEM_TYPE_BUTT
} smem_bm_mem_type;

/**
 * @brief CPU initiated data operation type, currently only support SDMA
 */
typedef enum {
    SMEMB_DATA_OP_SDMA = 1U << 0,
    SMEMB_DATA_OP_HOST_RDMA = 1U << 1,
    SMEMB_DATA_OP_HOST_TCP = 1U << 2,
    SMEMB_DATA_OP_DEVICE_RDMA = 1U << 3,
    SMEMB_DATA_OP_BUTT
} smem_bm_data_op_type;

/**
 * @brief Data copy direction
 */
typedef enum {
    SMEMB_COPY_L2G = 0, /* copy data from local space to global space */
    SMEMB_COPY_G2L = 1, /* copy data from global space to local space */
    SMEMB_COPY_G2H = 2, /* copy data from global space to host memory */
    SMEMB_COPY_H2G = 3, /* copy data from host memory to global space */
    SMEMB_COPY_G2G = 4, /* copy data from global space to global space */
    /* add here */
    SMEMB_COPY_BUTT
} smem_bm_copy_type;

typedef struct {
    void** sources;
    void** destinations;
    const uint64_t* dataSizes;
    uint32_t batchSize;
} smem_batch_copy_params;

#define TLS_PATH_SIZE 256
#define ASYNC_COPY_FLAG (1UL << (0))
typedef struct {
    bool tlsEnable;
    char caPath[TLS_PATH_SIZE];
    char crlPath[TLS_PATH_SIZE];
    char certPath[TLS_PATH_SIZE];
    char keyPath[TLS_PATH_SIZE];
    char keyPassPath[TLS_PATH_SIZE];
    char packagePath[TLS_PATH_SIZE];
    char decrypterLibPath[TLS_PATH_SIZE];
} smem_tls_config;

typedef struct {
    uint32_t initTimeout;   /* func smem_bm_init timeout, default 120s (min=1,
                               max=SMEM_BM_TIMEOUT_MAX) */
    uint32_t createTimeout; /* func smem_bm_create timeout, default 120s (min=1,
                               max=SMEM_BM_TIMEOUT_MAX) */
    uint32_t controlOperationTimeout; /* control operation timeout, default 120s
                                         (min=1, max=SMEM_BM_TIMEOUT_MAX) */
    bool startConfigStoreServer; /* whether to start config store, default true
                                  */
    bool startConfigStoreOnly;   /* only start the config store */
    bool dynamicWorldSize;       /* member cannot join dynamically */
    bool unifiedAddressSpace;    /* unified address with SVM */
    bool autoRanking; /* automatically allocate rank IDs, default is false. */
    uint16_t
        rankId;     /* user specified rank ID, valid for autoRanking is False */
    uint32_t flags; /* other flag, default 0 */
    char hcomUrl[64];
    smem_tls_config hcomTlsConfig;
    smem_tls_config storeTlsConfig;
} smem_bm_config_t;

typedef enum {
    SMEM_TRANS_NONE = 0, /* no role */
    SMEM_TRANS_SENDER,   /* sender */
    SMEM_TRANS_RECEIVER, /* receiver */
    SMEM_TRANS_BOTH,     /* both sender and receiver */
    SMEM_TRANS_BUTT
} smem_trans_role_t;

typedef struct {
    smem_trans_role_t role; /* transfer role */
    uint32_t initTimeout;   /* func timeout, default 120 seconds */
    uint32_t deviceId;      /* npu device id */
    uint32_t flags;         /* optional flags */
    smem_bm_data_op_type dataOpType;  /* data operation type */
    bool startConfigServer; /* whether to start config store, default false */
} smem_trans_config_t;

using smem_bm_t = void*;
using smem_trans_t = void*;

// func
using FUNC_SMEM_BM_CONFIG_INIT = int32_t (*)(smem_bm_config_t* config);
using FUNC_SMEM_BM_INIT = int32_t (*)(const char* storeURL, uint32_t worldSize,
                                      uint16_t deviceId,
                                      const smem_bm_config_t* config);
using FUNC_SMEM_BM_UNINIT = void (*)(uint32_t flags);
using FUNC_SMEM_BM_GET_RANK_ID = uint32_t (*)(void);
using FUNC_SMEM_BM_CREATE = smem_bm_t (*)(uint32_t id, uint32_t memberSize,
                                          smem_bm_data_op_type dataOpType,
                                          uint64_t localDRAMSize,
                                          uint64_t localHBMSize,
                                          uint32_t flags);
using FUNC_SMEM_BM_DESTROY = void (*)(smem_bm_t handle);
using FUNC_SMEM_BM_JOIN = int32_t (*)(smem_bm_t handle, uint32_t flags);
using FUNC_SMEM_BM_LEAVE = int32_t (*)(smem_bm_t handle, uint32_t flags);
using FUNC_SMEM_BM_GET_LOCAL_MEM_SIZE_BY_MEM_TYPE =
    uint64_t (*)(smem_bm_t handle, smem_bm_mem_type memType);
using FUNC_SMEM_BM_PTR_BY_MEM_TYPE = void* (*)(smem_bm_t handle,
                                               smem_bm_mem_type memType,
                                               uint16_t peerRankId);
using FUNC_SMEM_BM_COPY = int32_t (*)(smem_bm_t handle, const void* src,
                                      void* dest, uint64_t size,
                                      smem_bm_copy_type t, uint32_t flags);
using FUNC_SMEM_BM_COPY_BATCH = int32_t (*)(smem_bm_t handle,
                                            smem_batch_copy_params* params,
                                            smem_bm_copy_type t,
                                            uint32_t flags);
using FUNC_SMEM_BM_WAIT = int32_t (*)(smem_bm_t handle);
using FUNC_SMEM_BM_REGISTER_USER_MEM = int32_t (*)(smem_bm_t handle,
                                                   uint64_t addr,
                                                   uint64_t size);

using FUNC_SMEM_SET_EXTERN_LOGGER = int32_t (*)(void (*func)(int level,
                                                             const char* msg));

using FUNC_SMEM_SET_LOG_LEVEL = int32_t (*)(int level);

using FUNC_SMEM_CREATE_CONFIG_STORE = int32_t (*)(const char* storeUrl);

using FUNC_SMEM_TRANS_INIT = int32_t (*)(const smem_trans_config_t *config);
using FUNC_SMEM_TRANS_UNINIT = void (*)(uint32_t flags);
using FUNC_SMEM_TRANS_CREATE = smem_trans_t (*)(const char *, const char *, const smem_trans_config_t *);
using FUNC_SMEM_TRANS_DESTORY = void (*)(smem_trans_t, uint32_t);
using FUNC_SMEM_TRANS_REGISTER_MEM = int32_t (*)(smem_trans_t, void *, size_t, uint32_t);
using FUNC_SMEM_TRANS_UNREGISTER_MEM = int32_t (*)(smem_trans_t, void *[]);
using FUNC_SMEM_TRANS_BATCH_REGISTER_MEM = int32_t (*)(smem_trans_t, void *[], size_t[], uint32_t, uint32_t);
using FUNC_SMEM_TRANS_WRITE = int32_t (*)(smem_trans_t, const void *, const char *, void *, size_t);
using FUNC_SMEM_TRANS_BATCH_WRITE = int32_t (*)(smem_trans_t, const void *[], const char *,
                                                void *[], size_t [], uint32_t);
using FUNC_SMEM_TRANS_BATCH_READ = int32_t (*)(smem_trans_t, const void *[], const char *,
                                               void *[], size_t [], uint32_t);

#define DLSYM(TARGET_FUNC_VAR, TARGET_FUNC_TYPE, FILE_HANDLE, SYMBOL_NAME)   \
    do {                                                                     \
        TARGET_FUNC_VAR = (TARGET_FUNC_TYPE)dlsym(FILE_HANDLE, SYMBOL_NAME); \
        if ((TARGET_FUNC_VAR) == nullptr) {                                  \
            LOG(ERROR) << "Failed to call dlsym to load" << SYMBOL_NAME      \
                       << ", error" << dlerror();                            \
            dlclose(FILE_HANDLE);                                            \
            return -1;                                                       \
        }                                                                    \
    } while (0)

#define SMEM_BM_INIT_GVM_FLAG (1ULL << 1ULL)

constexpr uint64_t GB_MEM_BYTES = 1024ULL * 1024ULL * 1024ULL;
constexpr uint64_t TB_MEM_BYTES = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr uint64_t MAX_DRAM_MEM_BYTES = 32 * 1024ULL * 1024ULL * 1024ULL * 1024ULL;

struct MemFabricConfig {
    uint32_t deviceId{0};
    uint32_t worldSize{1024};
    uint64_t dramSize{GB_MEM_BYTES};
    uint64_t hbmSize{0};
    int32_t logLevel{1};
    bool useLocalHostMemory{false};
    smem_bm_data_op_type opType{SMEMB_DATA_OP_DEVICE_RDMA};
    smem_bm_data_op_type transOpType{SMEMB_DATA_OP_DEVICE_RDMA};
    std::string storeUrl{};
    // for smem trans
    smem_trans_role_t role{SMEM_TRANS_NONE};
};

class MemFabricSmemDl {
   public:
    static smem_bm_t GetSmemBmHandle()
    {
        return smemBmHandle_;
    }

    static smem_bm_t GetSmemTransHandle()
    {
        return smemTransHandle_;
    }

    static smem_type GetSmemTypeFlag()
    {
        return smemTypeFlag_;
    }

    static void SetSmemTypeFlag(smem_type smemTypeFlag)
    {
        smemTypeFlag_ = smemTypeFlag;
    }

    static int32_t SmemBmConfigInit(smem_bm_config_t* config) {
        if (!pSmemBmConfigInit) {
            LOG(ERROR) << "Call pSmemBmConfigInit is nullptr";
            return -1;
        }
        return pSmemBmConfigInit(config);
    }

    static int32_t SmemBmInit(const char* storeURL, uint32_t worldSize,
                              uint16_t deviceId,
                              const smem_bm_config_t* config) {
        if (!pSmemBmInit) {
            LOG(ERROR) << "Call pSmemBmInit is nullptr";
            return -1;
        }
        return pSmemBmInit(storeURL, worldSize, deviceId, config);
    }

    static void SmemBmUninit(uint32_t flags) {
        if (!pSmemBmUninit) {
            LOG(ERROR) << "Call pSmemBmUninit is nullptr";
            return;
        }
        pSmemBmUninit(flags);
    }

    static uint32_t SmemGetRankId() {
        if (!pSmemGetRankId) {
            LOG(ERROR) << "Call pSmemGetRankId is nullptr";
            return ~(0u);
        }
        return pSmemGetRankId();
    }

    static smem_bm_t SmemBmCreate(uint32_t id, uint32_t memberSize,
                                  smem_bm_data_op_type dataOpType,
                                  uint64_t localDRAMSize, uint64_t localHBMSize,
                                  uint32_t flags) {
        if (!pSmemBmCreate) {
            LOG(ERROR) << "Call pSmemBmCreate is nullptr";
            return nullptr;
        }
        smemBmHandle_ = pSmemBmCreate(id, memberSize, dataOpType, localDRAMSize,
                                      localHBMSize, flags);
        return smemBmHandle_;
    }

    static void SmemBmDestory(smem_bm_t handle) {
        if (!pSmemBmDestory) {
            LOG(ERROR) << "Call pSmemBmDestory is nullptr";
            return;
        }
        pSmemBmDestory(handle);
    }

    static int32_t SmemBmJoin(smem_bm_t handle, uint32_t flags) {
        if (!pSmemBmJoin) {
            LOG(ERROR) << "Call pSmemBmJoin is nullptr";
            return -1;
        }
        return pSmemBmJoin(handle, flags);
    }

    static int32_t SmemBmLeave(smem_bm_t handle, uint32_t flags) {
        if (!pSmemBmLeave) {
            LOG(ERROR) << "Call pSmemBmLeave is nullptr";
            return -1;
        }
        return pSmemBmLeave(handle, flags);
    }

    static uint64_t SmemBmGetLocalMemSizeByMemType(smem_bm_t handle,
                                                   smem_bm_mem_type memType) {
        if (!pSmemBmGetLocalMemSizeByMemType) {
            LOG(ERROR) << "Call pSmemBmGetLocalMemSizeByMemType is nullptr";
            return ~(0ull);
        }
        return pSmemBmGetLocalMemSizeByMemType(handle, memType);
    }

    static void* SmemBmPtrByMemType(smem_bm_t handle, smem_bm_mem_type memType,
                                    uint16_t peerRankId) {
        if (!pSmemBmPtrByMemType) {
            LOG(ERROR) << "Call pSmemBmPtrByMemType is nullptr";
            return nullptr;
        }
        return pSmemBmPtrByMemType(handle, memType, peerRankId);
    }

    static int32_t SmemBmCopy(smem_bm_t handle, const void* src, void* dest,
                              uint64_t size, smem_bm_copy_type t,
                              uint32_t flags) {
        if (!pSmemBmCopy) {
            LOG(ERROR) << "Call pSmemBmCopy is nullptr";
            return -1;
        }
        return pSmemBmCopy(handle, src, dest, size, t, flags);
    }

    static int32_t SmemBmCopyBatch(smem_bm_t handle,
                                   smem_batch_copy_params* params,
                                   smem_bm_copy_type t, uint32_t flags) {
        if (!pSmemBmCopyBatch) {
            LOG(ERROR) << "Call pSmemBmCopyBatch is nullptr";
            return -1;
        }
        return pSmemBmCopyBatch(handle, params, t, flags);
    }

    static int32_t SmemBmWait(smem_bm_t handle) {
        if (!pSmemBmWait) {
            LOG(ERROR) << "Call pSmemBmWait is nullptr";
            return -1;
        }
        return pSmemBmWait(handle);
    }

    static int32_t SmemBmRegisterUserMem(smem_bm_t handle, uint64_t addr,
                                         uint64_t size) {
        if (!pSmemBmRegisterUserMem) {
            LOG(ERROR) << "Call pSmemBmRegisterUserMem is nullptr";
            return -1;
        }
        return pSmemBmRegisterUserMem(handle, addr, size);
    }

    static int32_t SmemSetExternLogger(void (*func)(int level,
                                                    const char* msg)) {
        if (!pSmemSetExternLogger) {
            LOG(ERROR) << "Call pSmemSetExternLogger is nullptr";
            return -1;
        }
        return pSmemSetExternLogger(func);
    }

    static int32_t SmemSetLogLevel(int level) {
        if (!pSmemSetLogLevel) {
            LOG(ERROR) << "Call pSmemSetLogLevel is nullptr";
            return -1;
        }
        return pSmemSetLogLevel(level);
    }

    static int32_t SmemCreateConfigStore(const char* storeUrl) {
        if (!pSmemCreateConfigStore) {
            LOG(ERROR) << "Call pSmemCreateConfigStore is nullptr";
            return -1;
        }
        return pSmemCreateConfigStore(storeUrl);
    }

    static MemFabricConfig GetMemFabricConfig()
    {
        return config_;
    }

    static int32_t SmemTranInit(const smem_trans_config_t *config) {
        if (!pSmemTransInit) {

            LOG(ERROR) << "Call pSmemTransInit is nullptr";
            return -1;
        }
        return pSmemTransInit(config);
    }

    static void SmemTranUnInit(uint32_t flags) {
        if (!pSmemTransUnInit) {
            LOG(ERROR) << "Call pSmemTransUnInit is nullptr";
            return;
        }
        return pSmemTransUnInit(flags);
    }

    static smem_trans_t SmemTransCreate(const char *storeUrl, const char *uniqueId, const smem_trans_config_t *config) {
        if (!pSmemTransCreate) {
            LOG(ERROR) << "Call pSmemTransCreate is nullptr";
            return nullptr;
        }
        smemTransHandle_ = pSmemTransCreate(storeUrl, uniqueId, config);
        return smemTransHandle_;
    }

    static void SmemTransDestroy(smem_trans_t trans, uint32_t flags) {
        if (!pSmemTransDestory) {
            LOG(ERROR) << "Call pSmemTransDestroy is nullptr";
            return;
        }
        return pSmemTransDestory(trans, flags);
    }

    static int32_t SmemTransRegisterMem(smem_trans_t trans, void *addr, size_t length, uint32_t flags = 0) {
        if (!pSmemTransRegisterMem) {
            LOG(ERROR) << "Call pSmemTransRegisterMem is nullptr";
            return -1;
        }
        return pSmemTransRegisterMem(trans, addr, length, flags);
    }

    static int32_t SmemTransUnregisterMem(smem_trans_t trans, void *addr[]) {
        if (!pSmemTransUnregisterMem) {
            LOG(ERROR) << "Call pSmemTransUnregisterMem is nullptr";
            return -1;
        }
        return pSmemTransUnregisterMem(trans, addr);
    }

    static int32_t SmemTransBatchRegisterMem(smem_trans_t trans, void *addr[], size_t length[],
                                             uint32_t count, uint32_t flags) {
        if (!pSmemTransBatchRegisterMem) {
            LOG(ERROR) << "Call pSmemTransBatchRegisterMem is nullptr";
            return -1;
        }
        return pSmemTransBatchRegisterMem(trans, addr, length, count, flags);
    }

    static int32_t SmemTransWrite(smem_trans_t trans, const void *src, const char *destName, void *dest, size_t length) {
        if (!pSmemTransWrite) {
            LOG(ERROR) << "Call pSmemTransWrite is nullptr";
            return -1;
        }
        return pSmemTransWrite(trans, src, destName, dest, length);
    }

    static int32_t SmemTransBatchWrite(smem_trans_t trans, const void *src[], const char *destName, void *dest[],
                                       size_t length[], uint32_t count) {
        if (!pSmemTransBatchWrite) {
            LOG(ERROR) << "Call pSmemTransBatchWrite is nullptr";
            return -1;
        }
        return pSmemTransBatchWrite(trans, src, destName, dest, length, count);
    }

    static int32_t SmemTransBatchRead(smem_trans_t trans, const void *locals[], const char *remoteName, void *remotes[],
                                      size_t length[], uint32_t count) {
        if (!pSmemTransBatchRead) {
            LOG(ERROR) << "Call pSmemTransBatchWrite is nullptr";
            return -1;
        }
        return pSmemTransBatchRead(trans, locals, remoteName, remotes, length, count);
    }

    static void InitMemFabricConfig()
    {
        auto deviceIdStr = std::getenv("MF_DEVICE_ID");
        if (deviceIdStr) {
            auto val = atoi(deviceIdStr);
            if (val >= 0 && val < 32) {
                config_.deviceId = val;
            }
            LOG(WARNING) << "Set config deviceId=" << config_.deviceId
                         << " by environment variable MF_DEVICE_ID";
        }
        auto logLevelStr = std::getenv("MF_LOG_LEVEL");
        if (logLevelStr) {
            auto val = atoi(logLevelStr);
            if (val >= 0 && val < 5) {
                config_.logLevel = val;
            }
            LOG(WARNING) << "Set config logLevel=" << config_.logLevel
                         << " by environment variable MF_LOG_LEVEL";
        }
        auto dramSizeStr = std::getenv("MF_DRAM_SIZE");
        if (dramSizeStr) {
            uint64_t val = atol(dramSizeStr);
            if (val >= GB_MEM_BYTES && val <= TB_MEM_BYTES &&
                val % (GB_MEM_BYTES) == 0) {
                config_.dramSize = val;
            } else {
                LOG(ERROR) << "Get config dramSize failed size must in 1GB ~ 1TB"
                           << " and must align 1GB " << " now is:" << dramSizeStr;
            }
            LOG(WARNING) << "Set config dramSize=" << config_.dramSize
                         << " by environment variable MF_DRAM_SIZE";
        }
        config_.worldSize = std::min(config_.worldSize, (uint32_t) (MAX_DRAM_MEM_BYTES / config_.dramSize));
        auto useLocalHostMemoryStr = std::getenv("MF_USE_LOCAL_HOST");
        if (useLocalHostMemoryStr) {
            auto val = atoi(useLocalHostMemoryStr);
            if (val == 1) {
                config_.useLocalHostMemory = true;
            }
            LOG(WARNING) << "Set config useLocalHostMemory=" << config_.useLocalHostMemory
                         << " by environment variable MF_USE_LOCAL_HOST";
        }
        auto opTypeStr = std::getenv("MF_OP_TYPE");
        if (!opTypeStr) {
            config_.opType = SMEMB_DATA_OP_DEVICE_RDMA;
        } else if (std::string_view(opTypeStr) == "device_rdma") {
            config_.opType = SMEMB_DATA_OP_DEVICE_RDMA;
        } else if (std::string_view(opTypeStr) == "device_sdma") {
            config_.opType = SMEMB_DATA_OP_SDMA;
        } else {
            config_.opType = SMEMB_DATA_OP_BUTT;
            LOG(WARNING) << "Ignore value from environment variable MF_OP_TYPE";
        }
        auto transOpTypeStr = std::getenv("MF_PD_OP_TYPE");
        if (!transOpTypeStr) {
            config_.transOpType = SMEMB_DATA_OP_DEVICE_RDMA;
        } else if (std::string_view(opTypeStr) == "device_rdma") {
            config_.transOpType = SMEMB_DATA_OP_DEVICE_RDMA;
        } else if (std::string_view(opTypeStr) == "device_sdma") {
            config_.transOpType = SMEMB_DATA_OP_SDMA;
        } else {
            config_.transOpType = SMEMB_DATA_OP_BUTT;
            LOG(WARNING) << "Ignore value from environment variable MF_OP_TYPE";
        }
        LOG(WARNING) << "Set config transOpType=" << config_.transOpType
                     << " by environment variable MF_OP_TYPE";
        auto storeUrlStr = std::getenv("MF_STORE_URL");
        if (storeUrlStr) {
            config_.storeUrl = std::string_view(storeUrlStr);
        }
        LOG(ERROR) << "Set config storeUrl=" << config_.storeUrl
                     << " by environment variable MF_STORE_URL";
        auto roleStr = std::getenv("MF_SMEM_TRANS_ROLE");
        if (roleStr) {
            if (strcmp(roleStr, "Prefill") == 0) {
                config_.role = SMEM_TRANS_SENDER;
            } else if (strcmp(roleStr, "Decode") == 0) {
                config_.role = SMEM_TRANS_RECEIVER;
            } else {
                config_.role = SMEM_TRANS_BUTT;
                LOG(WARNING) << "Ignore value from environment variable MF_SMEM_TRANS_ROLE";
            }
        }
        LOG(WARNING) << "Set config roleStr=" << roleStr
                     << " by environment variable MF_SMEM_TRANS_ROLE";
    }

    static int32_t LoadMemFabricBmAPI() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (gLoaded_) {
            return 0;
        }
        InitMemFabricConfig();

        auto smemHandle = dlopen("libmf_smem.so", RTLD_NOW | RTLD_GLOBAL);
        if (smemHandle == nullptr) {
            LOG(ERROR) << "Failed to dlopen " << "libmf_smem.so" << " err: " << dlerror();
            return -1;
        }

        DLSYM(pSmemBmConfigInit, FUNC_SMEM_BM_CONFIG_INIT, smemHandle,
              "smem_bm_config_init");
        DLSYM(pSmemBmInit, FUNC_SMEM_BM_INIT, smemHandle, "smem_bm_init");
        DLSYM(pSmemBmUninit, FUNC_SMEM_BM_UNINIT, smemHandle, "smem_bm_uninit");
        DLSYM(pSmemGetRankId, FUNC_SMEM_BM_GET_RANK_ID, smemHandle,
              "smem_bm_get_rank_id");
        DLSYM(pSmemBmCreate, FUNC_SMEM_BM_CREATE, smemHandle, "smem_bm_create");
        DLSYM(pSmemBmDestory, FUNC_SMEM_BM_DESTROY, smemHandle,
              "smem_bm_destroy");
        DLSYM(pSmemBmJoin, FUNC_SMEM_BM_JOIN, smemHandle, "smem_bm_join");
        DLSYM(pSmemBmLeave, FUNC_SMEM_BM_LEAVE, smemHandle, "smem_bm_leave");
        DLSYM(pSmemBmGetLocalMemSizeByMemType,
              FUNC_SMEM_BM_GET_LOCAL_MEM_SIZE_BY_MEM_TYPE, smemHandle,
              "smem_bm_get_local_mem_size_by_mem_type");
        DLSYM(pSmemBmPtrByMemType, FUNC_SMEM_BM_PTR_BY_MEM_TYPE, smemHandle,
              "smem_bm_ptr_by_mem_type");
        DLSYM(pSmemBmCopy, FUNC_SMEM_BM_COPY, smemHandle, "smem_bm_copy");
        DLSYM(pSmemBmCopyBatch, FUNC_SMEM_BM_COPY_BATCH, smemHandle,
              "smem_bm_copy_batch");
        DLSYM(pSmemBmWait, FUNC_SMEM_BM_WAIT, smemHandle, "smem_bm_wait");
        DLSYM(pSmemBmRegisterUserMem, FUNC_SMEM_BM_REGISTER_USER_MEM,
              smemHandle, "smem_bm_register_user_mem");
        DLSYM(pSmemSetExternLogger, FUNC_SMEM_SET_EXTERN_LOGGER, smemHandle,
              "smem_set_extern_logger");
        DLSYM(pSmemSetLogLevel, FUNC_SMEM_SET_LOG_LEVEL, smemHandle,
              "smem_set_log_level");
        DLSYM(pSmemCreateConfigStore, FUNC_SMEM_CREATE_CONFIG_STORE, smemHandle,
              "smem_create_config_store");
        DLSYM(pSmemTransInit, FUNC_SMEM_TRANS_INIT, smemHandle, "smem_trans_init");
        DLSYM(pSmemTransUnInit, FUNC_SMEM_TRANS_UNINIT, smemHandle, "smem_trans_uninit");
        DLSYM(pSmemTransCreate, FUNC_SMEM_TRANS_CREATE, smemHandle, "smem_trans_create");
        DLSYM(pSmemTransDestory, FUNC_SMEM_TRANS_DESTORY, smemHandle, "smem_trans_destroy");
        DLSYM(pSmemTransRegisterMem, FUNC_SMEM_TRANS_REGISTER_MEM, smemHandle, "smem_trans_register_mem");
        DLSYM(pSmemTransUnregisterMem, FUNC_SMEM_TRANS_UNREGISTER_MEM, smemHandle, "smem_trans_deregister_mem");
        DLSYM(pSmemTransBatchRegisterMem, FUNC_SMEM_TRANS_BATCH_REGISTER_MEM,
              smemHandle, "smem_trans_batch_register_mem");
        DLSYM(pSmemTransWrite, FUNC_SMEM_TRANS_WRITE, smemHandle, "smem_trans_write");
        DLSYM(pSmemTransBatchWrite, FUNC_SMEM_TRANS_BATCH_WRITE, smemHandle, "smem_trans_batch_write");
        DLSYM(pSmemTransBatchRead, FUNC_SMEM_TRANS_BATCH_READ , smemHandle, "smem_trans_batch_read");

        gLoaded_ = true;
        LOG(INFO) << "load memfabric api success";
        return 0;
    }

private:
    static FUNC_SMEM_BM_CONFIG_INIT pSmemBmConfigInit;
    static FUNC_SMEM_BM_INIT pSmemBmInit;
    static FUNC_SMEM_BM_UNINIT pSmemBmUninit;
    static FUNC_SMEM_BM_GET_RANK_ID pSmemGetRankId;
    static FUNC_SMEM_BM_CREATE pSmemBmCreate;
    static FUNC_SMEM_BM_DESTROY pSmemBmDestory;
    static FUNC_SMEM_BM_JOIN pSmemBmJoin;
    static FUNC_SMEM_BM_LEAVE pSmemBmLeave;
    static FUNC_SMEM_BM_GET_LOCAL_MEM_SIZE_BY_MEM_TYPE
        pSmemBmGetLocalMemSizeByMemType;
    static FUNC_SMEM_BM_PTR_BY_MEM_TYPE pSmemBmPtrByMemType;
    static FUNC_SMEM_BM_COPY pSmemBmCopy;
    static FUNC_SMEM_BM_COPY_BATCH pSmemBmCopyBatch;
    static FUNC_SMEM_BM_WAIT pSmemBmWait;
    static FUNC_SMEM_BM_REGISTER_USER_MEM pSmemBmRegisterUserMem;
    static FUNC_SMEM_SET_EXTERN_LOGGER pSmemSetExternLogger;
    static FUNC_SMEM_SET_LOG_LEVEL pSmemSetLogLevel;
    static FUNC_SMEM_CREATE_CONFIG_STORE pSmemCreateConfigStore;
    static FUNC_SMEM_TRANS_INIT pSmemTransInit;
    static FUNC_SMEM_TRANS_UNINIT pSmemTransUnInit;
    static FUNC_SMEM_TRANS_CREATE pSmemTransCreate;
    static FUNC_SMEM_TRANS_DESTORY pSmemTransDestory;
    static FUNC_SMEM_TRANS_REGISTER_MEM pSmemTransRegisterMem;
    static FUNC_SMEM_TRANS_UNREGISTER_MEM pSmemTransUnregisterMem;
    static FUNC_SMEM_TRANS_BATCH_REGISTER_MEM pSmemTransBatchRegisterMem;
    static FUNC_SMEM_TRANS_WRITE pSmemTransWrite;
    static FUNC_SMEM_TRANS_BATCH_WRITE pSmemTransBatchWrite;
    static FUNC_SMEM_TRANS_BATCH_READ pSmemTransBatchRead;

   private:
    static bool gLoaded_;
    static std::mutex mutex_;
    static smem_bm_t smemBmHandle_;
    static smem_bm_t smemTransHandle_;
    static MemFabricConfig config_;
    static smem_type smemTypeFlag_;

    static inline bool IsSymlink(const std::string& filePath) {
        /* remove / at tail */
        std::string cleanPath = filePath;
        while (!cleanPath.empty() && cleanPath.back() == '/') {
            cleanPath.pop_back();
        }

        struct stat buf;
        if (lstat(cleanPath.c_str(), &buf) != 0) {
            return false;
        }
        return S_ISLNK(buf.st_mode);
    }

    static inline bool Realpath(std::string& path) {
        if (path.empty() || path.size() > PATH_MAX) {
            return false;
        }

        /* It will allocate memory to store path */
        char* tmp = new (std::nothrow) char[PATH_MAX + 1];
        char* realPath = realpath(path.c_str(), tmp);
        if (realPath == nullptr) {
            delete[] tmp;
            return false;
        }

        path = realPath;
        realPath = nullptr;
        delete[] tmp;
        return true;
    }

    static int GetLibPath(std::string& libDir, std::string& libPath) {
        if (IsSymlink(libDir)) {
            LOG(ERROR) << "Path for openssl library un-support symlink.";
            return -1;
        }

        if (!Realpath(libDir)) {
            LOG(ERROR) << "Path for openssl library is invalid.";
            return -1;
        }

        if (libDir.back() != '/') {
            libDir.push_back('/');
        }

        std::string tmpLibPath = libDir + "libsmem.so";
        if (::access(tmpLibPath.c_str(), F_OK) != 0) {
            LOG(ERROR) << "libssl.so path set in env is invalid";
            return -1;
        }
        libPath = std::move(tmpLibPath);
        return 0;
    }
};

// init config store
int MemFabricInitConfigStore(const std::string& store_address,
                             uint32_t storePort);
// init smem bm
int MemFabricInitSmemBm(int deviceId);
// init smem trans
int MemFabricInitSmemTrans(const std::string& sessionId , int deviceId);
// get segment info
std::pair<void*, size_t> MemFabricGetSegment();
}