#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/maxine/nvcv_types.h"
#include "core/util/dynlib.h"

// Minimal NVIDIA NvAR ABI surface.
//
// StudioCast does not vendor proprietary NVIDIA headers.
// We declare the subset of types / symbols we need and load the real
// implementations at runtime via dlopen(3) / dlsym(3).

// Selector string helpers (match the SDK's macro naming scheme).
#define NvAR_Parameter_Input(Name) "NvAR_Parameter_Input_" #Name
#define NvAR_Parameter_Output(Name) "NvAR_Parameter_Output_" #Name
#define NvAR_Parameter_Config(Name) "NvAR_Parameter_Config_" #Name
#define NvAR_Parameter_InOut(Name) "NvAR_Parameter_InOut_" #Name

namespace studiocast::maxine::ar {

// Feature selector strings.
inline constexpr const char* NVAR_FEATURE_GAZE_REDIRECTION = "GazeRedirection";
inline constexpr const char* NVAR_FEATURE_FACE_BOX_DETECTION = "FaceBoxDetection";

using NvAR_ParameterSelector = const char*;
using NvAR_FeatureID = const char*;
using NvAR_FeatureHandle = void*;

// Runtime-loaded NvAR API.
//
// StudioCast does not link against Maxine or CUDA at build time; this loader
// discovers a shared object and resolves required symbols via dlsym.
class ArApi {
public:
    using NvAR_Create_t = NvCV_Status (*)(NvAR_FeatureID featureID, NvAR_FeatureHandle* handle);
    using NvAR_Load_t = NvCV_Status (*)(NvAR_FeatureHandle handle);
    using NvAR_Run_t = NvCV_Status (*)(NvAR_FeatureHandle handle);
    using NvAR_Destroy_t = NvCV_Status (*)(NvAR_FeatureHandle handle);

    using NvAR_SetU32_t = NvCV_Status (*)(NvAR_FeatureHandle handle, NvAR_ParameterSelector param, uint32_t val);
    using NvAR_SetS32_t = NvCV_Status (*)(NvAR_FeatureHandle handle, NvAR_ParameterSelector param, int32_t val);
    using NvAR_SetF32_t = NvCV_Status (*)(NvAR_FeatureHandle handle, NvAR_ParameterSelector param, float val);
    using NvAR_SetF64_t = NvCV_Status (*)(NvAR_FeatureHandle handle, NvAR_ParameterSelector param, double val);
    using NvAR_SetU64_t = NvCV_Status (*)(NvAR_FeatureHandle handle, NvAR_ParameterSelector param, uint64_t val);
    using NvAR_SetString_t = NvCV_Status (*)(NvAR_FeatureHandle handle, NvAR_ParameterSelector param, const char* str);
    using NvAR_SetObject_t = NvCV_Status (*)(NvAR_FeatureHandle handle,
                                            NvAR_ParameterSelector param,
                                            void* ptr,
                                            unsigned long typeSize);
    using NvAR_SetF32Array_t = NvCV_Status (*)(NvAR_FeatureHandle handle, const char* name, float* vals, int count);

    using NvAR_GetU32_t = NvCV_Status (*)(NvAR_FeatureHandle handle, NvAR_ParameterSelector param, uint32_t* val);
    using NvAR_GetS32_t = NvCV_Status (*)(NvAR_FeatureHandle handle, NvAR_ParameterSelector param, int32_t* val);
    using NvAR_GetF32_t = NvCV_Status (*)(NvAR_FeatureHandle handle, NvAR_ParameterSelector param, float* val);
    using NvAR_GetF64_t = NvCV_Status (*)(NvAR_FeatureHandle handle, NvAR_ParameterSelector param, double* val);
    using NvAR_GetU64_t = NvCV_Status (*)(NvAR_FeatureHandle handle, NvAR_ParameterSelector param, uint64_t* val);
    using NvAR_GetString_t = NvCV_Status (*)(NvAR_FeatureHandle handle, NvAR_ParameterSelector param, const char** str);
    using NvAR_GetObject_t = NvCV_Status (*)(NvAR_FeatureHandle handle,
                                            NvAR_ParameterSelector param,
                                            void** ptr,
                                            unsigned long typeSize);
    using NvAR_GetF32Array_t = NvCV_Status (*)(NvAR_FeatureHandle handle,
                                              const char* name,
                                              const float** vals,
                                              int* count);

    // CUDA stream helpers (optional when present).
    using NvAR_CudaStreamCreate_t = NvCV_Status (*)(CUstream* stream);
    using NvAR_CudaStreamDestroy_t = NvCV_Status (*)(CUstream stream);
    using NvAR_SetCudaStream_t = NvCV_Status (*)(NvAR_FeatureHandle handle, NvAR_ParameterSelector param, CUstream stream);
    using NvAR_GetCudaStream_t = NvCV_Status (*)(NvAR_FeatureHandle handle, NvAR_ParameterSelector param, CUstream* stream);

    using NvAR_GetVersion_t = NvCV_Status (*)(uint32_t* version);
    using NvCV_GetErrorStringFromCode_t = const char* (*)(NvCV_Status code);

    struct Functions {
        NvAR_Create_t NvAR_Create = nullptr;
        NvAR_Load_t NvAR_Load = nullptr;
        NvAR_Run_t NvAR_Run = nullptr;
        NvAR_Destroy_t NvAR_Destroy = nullptr;

        NvAR_SetU32_t NvAR_SetU32 = nullptr;
        NvAR_SetS32_t NvAR_SetS32 = nullptr;
        NvAR_SetF32_t NvAR_SetF32 = nullptr;
        NvAR_SetF64_t NvAR_SetF64 = nullptr;
        NvAR_SetU64_t NvAR_SetU64 = nullptr;
        NvAR_SetString_t NvAR_SetString = nullptr;
        NvAR_SetObject_t NvAR_SetObject = nullptr;
        NvAR_SetF32Array_t NvAR_SetF32Array = nullptr;

        NvAR_GetU32_t NvAR_GetU32 = nullptr;
        NvAR_GetS32_t NvAR_GetS32 = nullptr;
        NvAR_GetF32_t NvAR_GetF32 = nullptr;
        NvAR_GetF64_t NvAR_GetF64 = nullptr;
        NvAR_GetU64_t NvAR_GetU64 = nullptr;
        NvAR_GetString_t NvAR_GetString = nullptr;
        NvAR_GetObject_t NvAR_GetObject = nullptr;
        NvAR_GetF32Array_t NvAR_GetF32Array = nullptr;

        // Optional CUDA stream helpers.
        NvAR_CudaStreamCreate_t NvAR_CudaStreamCreate = nullptr;
        NvAR_CudaStreamDestroy_t NvAR_CudaStreamDestroy = nullptr;
        NvAR_SetCudaStream_t NvAR_SetCudaStream = nullptr;
        NvAR_GetCudaStream_t NvAR_GetCudaStream = nullptr;

        // Optional misc.
        NvAR_GetVersion_t NvAR_GetVersion = nullptr;
        NvCV_GetErrorStringFromCode_t NvCV_GetErrorStringFromCode = nullptr;
    };

    ArApi();
    ~ArApi();

    ArApi(const ArApi&) = delete;
    ArApi& operator=(const ArApi&) = delete;

    ArApi(ArApi&&) noexcept;
    ArApi& operator=(ArApi&&) noexcept;

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

}  // namespace studiocast::maxine::ar
