#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "core/config/settings.h"
#include "core/maxine/gpu_selection.h"
#include "core/maxine/nvcv_api.h"
#include "core/maxine/vfx/vfx_types.h"
#include "core/util/dynlib.h"

namespace studiocast::maxine::vfx {

struct VfxApi {
    // Video Effects API
    NvVFX_CreateEffect_t NvVFX_CreateEffect = nullptr;
    NvVFX_DestroyEffect_t NvVFX_DestroyEffect = nullptr;
    NvVFX_CudaStreamCreate_t NvVFX_CudaStreamCreate = nullptr;
    NvVFX_CudaStreamDestroy_t NvVFX_CudaStreamDestroy = nullptr;
    NvVFX_SetCudaStream_t NvVFX_SetCudaStream = nullptr;
    NvVFX_GetCudaStream_t NvVFX_GetCudaStream = nullptr;
    NvVFX_Load_t NvVFX_Load = nullptr;
    NvVFX_Run_t NvVFX_Run = nullptr;

    NvVFX_SetImage_t NvVFX_SetImage = nullptr;
    NvVFX_SetString_t NvVFX_SetString = nullptr;
    NvVFX_GetString_t NvVFX_GetString = nullptr;
    NvVFX_SetF32_t NvVFX_SetF32 = nullptr;
    NvVFX_SetU32_t NvVFX_SetU32 = nullptr;
    NvVFX_SetS32_t NvVFX_SetS32 = nullptr;

    NvVFX_GetU32_t NvVFX_GetU32 = nullptr;
    NvVFX_GetS32_t NvVFX_GetS32 = nullptr;
    NvVFX_GetF32_t NvVFX_GetF32 = nullptr;

    NvVFX_SetObject_t NvVFX_SetObject = nullptr;
    NvVFX_GetObject_t NvVFX_GetObject = nullptr;

    NvVFX_AllocateState_t NvVFX_AllocateState = nullptr;
    NvVFX_DeallocateState_t NvVFX_DeallocateState = nullptr;
    NvVFX_ResetState_t NvVFX_ResetState = nullptr;
    NvVFX_SetStateObjectHandleArray_t NvVFX_SetStateObjectHandleArray = nullptr;

    // NvCVImage API
    NvCVImage_Init_t NvCVImage_Init = nullptr;
    NvCVImage_Alloc_t NvCVImage_Alloc = nullptr;
    NvCVImage_Realloc_t NvCVImage_Realloc = nullptr;
    NvCVImage_Dealloc_t NvCVImage_Dealloc = nullptr;
    NvCVImage_Transfer_t NvCVImage_Transfer = nullptr;
    NvCVImage_CompositeOverConstant_t NvCVImage_CompositeOverConstant = nullptr;
    NvCVImage_Composite_t NvCVImage_Composite = nullptr;

    // Error strings
    NvCV_GetErrorStringFromCode_t NvCV_GetErrorStringFromCode = nullptr;
};

struct VfxDiagnostics {
    bool initialized = false;
    std::string error;

    std::optional<studiocast::maxine::SelectedGpu> selected_gpu;

    std::filesystem::path sdk_root;
    std::filesystem::path models_dir;

    std::filesystem::path vfx_library;
    std::filesystem::path nvcv_library;

    bool cuda_runtime_loaded = false;
};

class VfxRuntime {
public:
    VfxRuntime();
    ~VfxRuntime();

    VfxRuntime(const VfxRuntime&) = delete;
    VfxRuntime& operator=(const VfxRuntime&) = delete;

    bool Initialize(const config::GpuSelection& gpu_policy, std::string* error_out);

    bool IsInitialized() const { return diag_.initialized; }

    const VfxApi& api() const { return api_; }
    CUstream stream() const { return stream_; }

    const VfxDiagnostics& diagnostics() const { return diag_; }

private:
    util::DynLib vfx_lib_;
    util::DynLib cuda_lib_;

    NvcvApi nvcv_api_;

    VfxApi api_{};
    VfxDiagnostics diag_{};

    CUstream stream_ = nullptr;
};

}  // namespace studiocast::maxine::vfx
