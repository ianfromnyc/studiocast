#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/maxine/vfx/vfx_types.h"
#include "core/util/dynlib.h"

namespace studiocast::maxine::vfx {

// Effect selector strings (from VFX docs; used with NvVFX_CreateEffect).
inline constexpr const char* NVVFX_FX_GREEN_SCREEN = "Green Screen";
inline constexpr const char* NVVFX_FX_BGBLUR = "Background Blur";
inline constexpr const char* NVVFX_FX_DENOISING = "Denoising";

// Parameter selector strings (from VFX docs; used with NvVFX_Set*/Get*).
inline constexpr const char* NVVFX_MODEL_DIRECTORY = "modelDir";
inline constexpr const char* NVVFX_CUDA_STREAM = "cudaStream";
inline constexpr const char* NVVFX_STRENGTH = "strength";
inline constexpr const char* NVVFX_MODE = "mode";
inline constexpr const char* NVVFX_TEMPORAL = "temporal";
inline constexpr const char* NVVFX_STATE = "state";

inline constexpr const char* NVVFX_INPUT_IMAGE = "srcImage";
inline constexpr const char* NVVFX_OUTPUT_IMAGE = "dstImage";

// Runtime-loaded NvVFX API.
//
// StudioCast does not link against Maxine or CUDA at build time; this loader
// discovers a shared object and resolves required symbols via dlsym.
class VfxApi {
public:
    struct Functions {
        NvVFX_CreateEffect_t NvVFX_CreateEffect = nullptr;
        NvVFX_DestroyEffect_t NvVFX_DestroyEffect = nullptr;

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

        NvVFX_CudaStreamCreate_t NvVFX_CudaStreamCreate = nullptr;
        NvVFX_CudaStreamDestroy_t NvVFX_CudaStreamDestroy = nullptr;
        NvVFX_CudaStreamSynchronize_t NvVFX_CudaStreamSynchronize = nullptr;
        NvVFX_SetCudaStream_t NvVFX_SetCudaStream = nullptr;
        NvVFX_GetCudaStream_t NvVFX_GetCudaStream = nullptr;

        NvCV_GetErrorStringFromCode_t NvCV_GetErrorStringFromCode = nullptr;
    };

    VfxApi();
    ~VfxApi();

    VfxApi(const VfxApi&) = delete;
    VfxApi& operator=(const VfxApi&) = delete;

    VfxApi(VfxApi&&) noexcept;
    VfxApi& operator=(VfxApi&&) noexcept;

    // Initialize using a default search set (user-local roots + /usr/local).
    bool Initialize(std::string* error_out);

    // Initialize using explicit SDK roots.
    bool Initialize(const std::vector<std::filesystem::path>& sdk_roots, std::string* error_out);

    // Initialize by dlopening a specific shared object path.
    bool InitializeFromLibraryPath(const std::filesystem::path& library_path, std::string* error_out);

    bool IsInitialized() const { return initialized_; }

    const std::filesystem::path& library_path() const { return library_path_; }
    const Functions& f() const { return f_; }
    const std::string& error() const { return error_; }

    std::string StatusToString(NvCV_Status code) const;

private:
    bool InitializeImpl(const std::vector<std::filesystem::path>& sdk_roots, std::string* error_out);
    bool InitializeFromLibraryPathImpl(const std::filesystem::path& library_path, std::string* error_out);
    bool LoadSymbols(std::string* error_out);

    bool initialized_ = false;
    std::filesystem::path library_path_;
    util::DynLib lib_;
    Functions f_{};
    std::string error_;
};

}  // namespace studiocast::maxine::vfx
