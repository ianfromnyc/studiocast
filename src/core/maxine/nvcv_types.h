#pragma once

// Minimal NVIDIA NvCVImage ABI surface.
//
// StudioCast is open-source and does not vendor proprietary NVIDIA headers.
// We declare only the types / constants we need and load the real
// implementations at runtime via dlopen(3) / dlsym(3).
//
// IMPORTANT: These declarations must match the NVIDIA SDK ABI.

#include <cstdint>

namespace studiocast::maxine {

// CUDA stream forward declaration.
// CUDA defines: typedef struct CUstream_st* CUstream;
struct CUstream_st;
using CUstream = CUstream_st *;

// In NVIDIA headers this is a signed integer type (often an enum).
using NvCV_Status = int32_t;
inline constexpr NvCV_Status NVCV_SUCCESS = 0;

// The format of pixels in an image.
// Values are part of the ABI.
enum NvCVImage_PixelFormat : uint32_t {
  NVCV_FORMAT_UNKNOWN = 0,
  NVCV_Y = 1,
  NVCV_A = 2,
  NVCV_YA = 3,
  NVCV_RGB = 4,
  NVCV_BGR = 5,
  NVCV_RGBA = 6,
  NVCV_BGRA = 7,
  NVCV_ARGB = 8,
  NVCV_ABGR = 9,
  NVCV_YUV420 = 10,
  NVCV_YUV422 = 11,
  NVCV_YUV444 = 12,
};

// The data type used to represent each component of an image.
// Values are part of the ABI.
enum NvCVImage_ComponentType : uint32_t {
  NVCV_TYPE_UNKNOWN = 0,
  NVCV_U8 = 1,
  NVCV_U16 = 2,
  NVCV_S16 = 3,
  NVCV_F16 = 4,
  NVCV_U32 = 5,
  NVCV_S32 = 6,
  NVCV_F32 = 7,
  NVCV_U64 = 8,
  NVCV_S64 = 9,
  NVCV_F64 = 10,
};

// Value for the planar field / layout argument.
// Note: the LSB can be used to distinguish between chunky and planar formats.
inline constexpr uint32_t NVCV_INTERLEAVED = 0;
inline constexpr uint32_t NVCV_CHUNKY = 0;
inline constexpr uint32_t NVCV_PLANAR = 1;

inline constexpr uint32_t NVCV_UYVY = 2;
inline constexpr uint32_t NVCV_VYUY = 4;
inline constexpr uint32_t NVCV_YUYV = 6;
inline constexpr uint32_t NVCV_YVYU = 8;
inline constexpr uint32_t NVCV_CYUV = 10;
inline constexpr uint32_t NVCV_CYVU = 12;

inline constexpr uint32_t NVCV_YUV = 3;
inline constexpr uint32_t NVCV_YVU = 5;
inline constexpr uint32_t NVCV_YCUV = 7;
inline constexpr uint32_t NVCV_YCVU = 9;

// FOURCC aliases for specific layouts.
inline constexpr uint32_t NVCV_I420 = NVCV_YUV;
inline constexpr uint32_t NVCV_IYUV = NVCV_YUV;
inline constexpr uint32_t NVCV_YV12 = NVCV_YVU;
inline constexpr uint32_t NVCV_NV12 = NVCV_YCUV;
inline constexpr uint32_t NVCV_NV21 = NVCV_YCVU;
inline constexpr uint32_t NVCV_YUY2 = NVCV_YUYV;

// Memory space constants (gpuMem field / memSpace argument).
inline constexpr uint32_t NVCV_CPU = 0;
inline constexpr uint32_t NVCV_GPU = 1;
inline constexpr uint32_t NVCV_CUDA = 1;
inline constexpr uint32_t NVCV_CPU_PINNED = 2;
inline constexpr uint32_t NVCV_CUDA_ARRAY = 3;

// NvCVImage POD struct used by the SDK.
// Note: field order matters for ABI compatibility.
struct NvCVImage {
  uint32_t width;
  uint32_t height;
  int32_t pitch;
  NvCVImage_PixelFormat pixelFormat;
  NvCVImage_ComponentType componentType;
  uint8_t pixelBytes;
  uint8_t componentBytes;
  uint8_t numComponents;
  uint8_t planar;     // NVCV_CHUNKY, NVCV_PLANAR, NVCV_UYVY, ...
  uint8_t gpuMem;     // NVCV_CPU, NVCV_CPU_PINNED, NVCV_CUDA, ...
  uint8_t colorspace; // OR of colorspace, range and chroma phase (YUV)
  uint8_t reserved[2];
  void *pixels;
  void *deletePtr;
  void (*deleteProc)(void *p);
  uint64_t bufferBytes;
};

} // namespace studiocast::maxine
