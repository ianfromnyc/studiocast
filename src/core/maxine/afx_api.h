#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "core/util/dynlib.h"

// Minimal NVIDIA NvAFX ABI surface.
//
// StudioCast does not vendor proprietary NVIDIA headers.
// We declare the subset of types / symbols we need and load the real
// implementations at runtime via dlopen(3) / dlsym(3).

namespace studiocast::maxine::afx {

using NvAFX_Status = int;
inline constexpr NvAFX_Status NVAFX_SUCCESS = 0;

using NvAFX_Handle = void*;
using NvAFX_ParameterSelector = const char*;
using NvAFX_EffectSelector = const char*;

// Runtime-loaded NvAFX API.
//
// StudioCast does not link against Maxine at build time; this loader discovers
// a shared object and resolves required symbols via dlsym.
class AfxApi {
public:
    using NvAFX_CreateEffect_t = NvAFX_Status (*)(NvAFX_EffectSelector effect, NvAFX_Handle* handle);
    using NvAFX_DestroyEffect_t = NvAFX_Status (*)(NvAFX_Handle handle);

    using NvAFX_SetU32_t = NvAFX_Status (*)(NvAFX_Handle handle, NvAFX_ParameterSelector param, std::uint32_t val);
    using NvAFX_SetU32List_t = NvAFX_Status (*)(NvAFX_Handle handle,
                                                NvAFX_ParameterSelector param,
                                                const std::uint32_t* vals,
                                                std::uint32_t count);
    using NvAFX_SetFloat_t = NvAFX_Status (*)(NvAFX_Handle handle, NvAFX_ParameterSelector param, float val);
    using NvAFX_SetString_t = NvAFX_Status (*)(NvAFX_Handle handle, NvAFX_ParameterSelector param, const char* str);

    using NvAFX_GetU32_t = NvAFX_Status (*)(NvAFX_Handle handle, NvAFX_ParameterSelector param, std::uint32_t* val);
    using NvAFX_GetU32List_t = NvAFX_Status (*)(NvAFX_Handle handle,
                                                NvAFX_ParameterSelector param,
                                                std::uint32_t* vals,
                                                std::uint32_t* count_in_out);

    using NvAFX_Load_t = NvAFX_Status (*)(NvAFX_Handle handle);

    // Signature depends on effect; most AFX effects are frame-based float PCM.
    // We keep the ABI minimal and will refine the signature once we integrate
    // actual AFX processing.
    using NvAFX_Run_t = NvAFX_Status (*)(NvAFX_Handle handle, const float* input, float* output, std::uint32_t num_samples);

    struct Functions {
        NvAFX_CreateEffect_t NvAFX_CreateEffect = nullptr;
        NvAFX_DestroyEffect_t NvAFX_DestroyEffect = nullptr;

        NvAFX_SetU32_t NvAFX_SetU32 = nullptr;
        NvAFX_SetU32List_t NvAFX_SetU32List = nullptr;  // optional
        NvAFX_SetFloat_t NvAFX_SetFloat = nullptr;
        NvAFX_SetString_t NvAFX_SetString = nullptr;

        NvAFX_GetU32_t NvAFX_GetU32 = nullptr;
        NvAFX_GetU32List_t NvAFX_GetU32List = nullptr;  // optional

        NvAFX_Load_t NvAFX_Load = nullptr;
        NvAFX_Run_t NvAFX_Run = nullptr;
    };

    AfxApi();
    ~AfxApi();

    AfxApi(const AfxApi&) = delete;
    AfxApi& operator=(const AfxApi&) = delete;

    AfxApi(AfxApi&&) noexcept;
    AfxApi& operator=(AfxApi&&) noexcept;

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

}  // namespace studiocast::maxine::afx
