#pragma once

// Minimal NVIDIA Maxine VFX ABI surface.
//
// StudioCast is open-source and does not vendor proprietary NVIDIA headers.
// We declare the minimal types / function-pointer types we need and load the
// real implementations at runtime via dlopen(3).

#include "../nvcv_types.h"

namespace studiocast::maxine::vfx {

// VFX types.
using NvVFX_Handle = void *;
using NvVFX_StateObjectHandle = void *;
using NvVFX_EffectSelector = const char *;
using NvVFX_ParameterSelector = const char *;

// VFX function pointer types.
using NvVFX_CreateEffect_t = NvCV_Status (*)(NvVFX_EffectSelector code,
                                             NvVFX_Handle *obj);
using NvVFX_DestroyEffect_t = void (*)(NvVFX_Handle obj);

using NvVFX_CudaStreamCreate_t = NvCV_Status (*)(CUstream *stream);
using NvVFX_CudaStreamDestroy_t = void (*)(CUstream stream);
using NvVFX_CudaStreamSynchronize_t = NvCV_Status (*)(CUstream stream);
using NvVFX_SetCudaStream_t = NvCV_Status (*)(NvVFX_Handle obj,
                                              NvVFX_ParameterSelector paramName,
                                              CUstream stream);
using NvVFX_GetCudaStream_t = NvCV_Status (*)(NvVFX_Handle obj,
                                              NvVFX_ParameterSelector paramName,
                                              CUstream *stream);

using NvVFX_Load_t = NvCV_Status (*)(NvVFX_Handle obj);
using NvVFX_Run_t = NvCV_Status (*)(NvVFX_Handle obj, int async);

using NvVFX_SetImage_t = NvCV_Status (*)(NvVFX_Handle obj,
                                         NvVFX_ParameterSelector paramName,
                                         NvCVImage *im);
using NvVFX_SetString_t = NvCV_Status (*)(NvVFX_Handle obj,
                                          NvVFX_ParameterSelector paramName,
                                          const char *str);
using NvVFX_GetString_t = NvCV_Status (*)(NvVFX_Handle obj,
                                          NvVFX_ParameterSelector paramName,
                                          const char **str);

using NvVFX_SetF32_t = NvCV_Status (*)(NvVFX_Handle obj,
                                       NvVFX_ParameterSelector paramName,
                                       float val);
using NvVFX_SetF64_t = NvCV_Status (*)(NvVFX_Handle obj,
                                       NvVFX_ParameterSelector paramName,
                                       double val);
using NvVFX_SetU32_t = NvCV_Status (*)(NvVFX_Handle obj,
                                       NvVFX_ParameterSelector paramName,
                                       uint32_t val);
using NvVFX_SetS32_t = NvCV_Status (*)(NvVFX_Handle obj,
                                       NvVFX_ParameterSelector paramName,
                                       int32_t val);

using NvVFX_GetU32_t = NvCV_Status (*)(NvVFX_Handle obj,
                                       NvVFX_ParameterSelector paramName,
                                       uint32_t *val);
using NvVFX_GetS32_t = NvCV_Status (*)(NvVFX_Handle obj,
                                       NvVFX_ParameterSelector paramName,
                                       int32_t *val);
using NvVFX_GetF32_t = NvCV_Status (*)(NvVFX_Handle obj,
                                       NvVFX_ParameterSelector paramName,
                                       float *val);

using NvVFX_SetObject_t = NvCV_Status (*)(NvVFX_Handle obj,
                                          NvVFX_ParameterSelector paramName,
                                          void *ptr);
using NvVFX_GetObject_t = NvCV_Status (*)(NvVFX_Handle obj,
                                          NvVFX_ParameterSelector paramName,
                                          void **ptr, unsigned long typeSize);

using NvVFX_AllocateState_t = NvCV_Status (*)(NvVFX_Handle obj,
                                              NvVFX_StateObjectHandle *handle);
using NvVFX_DeallocateState_t = NvCV_Status (*)(NvVFX_Handle obj,
                                                NvVFX_StateObjectHandle handle);
using NvVFX_ResetState_t = NvCV_Status (*)(NvVFX_Handle obj,
                                           NvVFX_StateObjectHandle handle);

using NvVFX_SetStateObjectHandleArray_t =
    NvCV_Status (*)(NvVFX_Handle obj, NvVFX_ParameterSelector paramName,
                    NvVFX_StateObjectHandle *handleArray, uint32_t count);

using NvCV_GetErrorStringFromCode_t = const char *(*)(NvCV_Status code);

// NvCVImage function pointer types.
using NvCVImage_Init_t = NvCV_Status (*)(NvCVImage *im, unsigned width,
                                         unsigned height, int pitch,
                                         void *pixels,
                                         NvCVImage_PixelFormat format,
                                         NvCVImage_ComponentType type,
                                         unsigned layout, unsigned memSpace);

using NvCVImage_Alloc_t = NvCV_Status (*)(NvCVImage *im, unsigned width,
                                          unsigned height,
                                          NvCVImage_PixelFormat format,
                                          NvCVImage_ComponentType type,
                                          unsigned layout, unsigned memSpace,
                                          unsigned alignment);

using NvCVImage_Realloc_t = NvCV_Status (*)(NvCVImage *im, unsigned width,
                                            unsigned height,
                                            NvCVImage_PixelFormat format,
                                            NvCVImage_ComponentType type,
                                            unsigned layout, unsigned memSpace,
                                            unsigned alignment);

using NvCVImage_Dealloc_t = NvCV_Status (*)(NvCVImage *im);

using NvCVImage_Transfer_t = NvCV_Status (*)(const NvCVImage *src,
                                             NvCVImage *dst, float scale,
                                             CUstream stream, NvCVImage *tmp);

using NvCVImage_CompositeOverConstant_t = NvCV_Status (*)(const NvCVImage *src,
                                                          const NvCVImage *mat,
                                                          const void *bgColor,
                                                          NvCVImage *dst,
                                                          CUstream stream);

using NvCVImage_Composite_t = NvCV_Status (*)(const NvCVImage *fg,
                                              const NvCVImage *bg,
                                              const NvCVImage *mat,
                                              NvCVImage *dst, CUstream stream);

} // namespace studiocast::maxine::vfx
