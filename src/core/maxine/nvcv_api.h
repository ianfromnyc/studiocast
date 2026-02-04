#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/maxine/nvcv_types.h"

namespace studiocast::maxine {

// Runtime-loaded NvCVImage API (used by Maxine VFX + AR).
//
// This is intentionally a small surface: it discovers and dlopens the shared
// library (commonly libnvcvimage.so) and resolves a minimal set of symbols.
class NvcvApi {
public:
    enum class Requirement {
        // Minimal set required by StudioCast effects.
        Minimal,
        // Superset used by the current VFX runtime scaffolding.
        VfxCompat,
    };

    using NvCV_GetErrorStringFromCode_t = const char* (*)(NvCV_Status code);

    using NvCVImage_Init_t = NvCV_Status (*)(NvCVImage* im,
                                             unsigned width,
                                             unsigned height,
                                             int pitch,
                                             void* pixels,
                                             NvCVImage_PixelFormat format,
                                             NvCVImage_ComponentType type,
                                             unsigned layout,
                                             unsigned memSpace);

    using NvCVImage_Alloc_t = NvCV_Status (*)(NvCVImage* im,
                                              unsigned width,
                                              unsigned height,
                                              NvCVImage_PixelFormat format,
                                              NvCVImage_ComponentType type,
                                              unsigned layout,
                                              unsigned memSpace,
                                              unsigned alignment);

    using NvCVImage_Realloc_t = NvCV_Status (*)(NvCVImage* im,
                                                unsigned width,
                                                unsigned height,
                                                NvCVImage_PixelFormat format,
                                                NvCVImage_ComponentType type,
                                                unsigned layout,
                                                unsigned memSpace,
                                                unsigned alignment);

    using NvCVImage_Dealloc_t = NvCV_Status (*)(NvCVImage* im);

    using NvCVImage_Transfer_t = NvCV_Status (*)(const NvCVImage* src,
                                                 NvCVImage* dst,
                                                 float scale,
                                                 CUstream stream,
                                                 NvCVImage* tmp);

    using NvCVImage_CompositeOverConstant_t = NvCV_Status (*)(const NvCVImage* src,
                                                              const NvCVImage* mat,
                                                              const void* bgColor,
                                                              NvCVImage* dst,
                                                              CUstream stream);

    using NvCVImage_Composite_t = NvCV_Status (*)(const NvCVImage* fg,
                                                  const NvCVImage* bg,
                                                  const NvCVImage* mat,
                                                  NvCVImage* dst,
                                                  CUstream stream);

    struct Functions {
        // Minimal
        NvCVImage_Alloc_t NvCVImage_Alloc = nullptr;
        NvCVImage_Dealloc_t NvCVImage_Dealloc = nullptr;
        NvCVImage_Transfer_t NvCVImage_Transfer = nullptr;
        NvCVImage_Composite_t NvCVImage_Composite = nullptr;

        // Extras (VFX scaffolding currently uses these)
        NvCVImage_Init_t NvCVImage_Init = nullptr;
        NvCVImage_Realloc_t NvCVImage_Realloc = nullptr;
        NvCVImage_CompositeOverConstant_t NvCVImage_CompositeOverConstant = nullptr;

        // Optional error strings
        NvCV_GetErrorStringFromCode_t NvCV_GetErrorStringFromCode = nullptr;
    };

    NvcvApi();
    ~NvcvApi();

    NvcvApi(const NvcvApi&) = delete;
    NvcvApi& operator=(const NvcvApi&) = delete;

    NvcvApi(NvcvApi&&) noexcept;
    NvcvApi& operator=(NvcvApi&&) noexcept;

    // Initialize using a default search set (user-local roots + /usr/local).
    bool Initialize(Requirement req, std::string* error_out);

    // Initialize using explicit SDK roots (e.g. VideoFX root and/or ARSDK root).
    bool Initialize(Requirement req,
                    const std::vector<std::filesystem::path>& sdk_roots,
                    std::string* error_out);

    // Initialize by dlopening a specific shared object path (used when another
    // Maxine component library also exports NvCVImage symbols).
    bool InitializeFromLibraryPath(Requirement req,
                                   const std::filesystem::path& library_path,
                                   std::string* error_out);

    bool IsInitialized() const { return initialized_; }

    const std::filesystem::path& library_path() const { return library_path_; }
    const Functions& f() const { return f_; }
    const std::string& error() const { return error_; }

private:
    bool InitializeImpl(Requirement req,
                        const std::vector<std::filesystem::path>& sdk_roots,
                        std::string* error_out);

    bool InitializeFromLibraryPathImpl(Requirement req,
                                       const std::filesystem::path& library_path,
                                       std::string* error_out);

    bool LoadSymbols(Requirement req, std::string* error_out);

    bool initialized_ = false;
    std::filesystem::path library_path_;
    Functions f_{};
    std::string error_;

    class Impl;
    Impl* impl_ = nullptr;
};

}  // namespace studiocast::maxine
