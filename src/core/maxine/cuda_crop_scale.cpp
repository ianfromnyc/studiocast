#include "core/maxine/cuda_crop_scale.h"

#include <algorithm>
#include <cstdint>

namespace studiocast::maxine {
namespace {

// Nearest-neighbor crop+scale kernel (BGR interleaved, U8).
// Parameters:
//   srcPtr, srcPitch, srcW, srcH,
//   dstPtr, dstPitch, dstW, dstH,
//   cropX, cropY, cropW, cropH
static constexpr const char* kCropScalePtx = R"ptx(
.version 6.0
.target sm_30
.address_size 64

.visible .entry crop_scale_bgr_u8(
    .param .u64 srcPtr,
    .param .u32 srcPitch,
    .param .u32 srcW,
    .param .u32 srcH,
    .param .u64 dstPtr,
    .param .u32 dstPitch,
    .param .u32 dstW,
    .param .u32 dstH,
    .param .f32 cropX,
    .param .f32 cropY,
    .param .f32 cropW,
    .param .f32 cropH
)
{
    .reg .pred  %p<4>;
    .reg .b32   %r<24>;
    .reg .b64   %rd<12>;
    .reg .f32   %f<16>;

    ld.param.u64 %rd1, [srcPtr];
    ld.param.u32 %r1, [srcPitch];
    ld.param.u32 %r2, [srcW];
    ld.param.u32 %r3, [srcH];
    ld.param.u64 %rd2, [dstPtr];
    ld.param.u32 %r4, [dstPitch];
    ld.param.u32 %r5, [dstW];
    ld.param.u32 %r6, [dstH];
    ld.param.f32 %f1, [cropX];
    ld.param.f32 %f2, [cropY];
    ld.param.f32 %f3, [cropW];
    ld.param.f32 %f4, [cropH];

    mov.u32 %r7, %ctaid.x;
    mov.u32 %r8, %ntid.x;
    mov.u32 %r9, %tid.x;
    mad.lo.s32 %r10, %r7, %r8, %r9; // x

    mov.u32 %r11, %ctaid.y;
    mov.u32 %r12, %ntid.y;
    mov.u32 %r13, %tid.y;
    mad.lo.s32 %r14, %r11, %r12, %r13; // y

    setp.ge.s32 %p1, %r10, %r5;
    setp.ge.s32 %p2, %r14, %r6;
    or.pred %p3, %p1, %p2;
    @%p3 bra DONE;

    // fx = cropX + (x + 0.5) * cropW / dstW
    cvt.rn.f32.s32 %f5, %r10;
    add.f32 %f5, %f5, 0f3F000000; // 0.5
    mul.f32 %f5, %f5, %f3;
    cvt.rn.f32.u32 %f6, %r5;
    div.rn.f32 %f5, %f5, %f6;
    add.f32 %f5, %f5, %f1;

    // fy = cropY + (y + 0.5) * cropH / dstH
    cvt.rn.f32.s32 %f7, %r14;
    add.f32 %f7, %f7, 0f3F000000; // 0.5
    mul.f32 %f7, %f7, %f4;
    cvt.rn.f32.u32 %f8, %r6;
    div.rn.f32 %f7, %f7, %f8;
    add.f32 %f7, %f7, %f2;

    cvt.rzi.s32.f32 %r15, %f5; // ix
    cvt.rzi.s32.f32 %r16, %f7; // iy

    max.s32 %r15, %r15, 0;
    max.s32 %r16, %r16, 0;
    add.s32 %r17, %r2, -1;
    add.s32 %r18, %r3, -1;
    min.s32 %r15, %r15, %r17;
    min.s32 %r16, %r16, %r18;

    // src addr
    mul.wide.s32 %rd3, %r16, %r1;
    mul.lo.s32 %r19, %r15, 3;
    cvt.u64.u32 %rd4, %r19;
    add.u64 %rd5, %rd1, %rd3;
    add.u64 %rd5, %rd5, %rd4;

    // dst addr
    mul.wide.s32 %rd6, %r14, %r4;
    mul.lo.s32 %r20, %r10, 3;
    cvt.u64.u32 %rd7, %r20;
    add.u64 %rd8, %rd2, %rd6;
    add.u64 %rd8, %rd8, %rd7;

    // load/store BGR
    ld.global.u8 %r21, [%rd5];
    ld.global.u8 %r22, [%rd5+1];
    ld.global.u8 %r23, [%rd5+2];
    st.global.u8 [%rd8], %r21;
    st.global.u8 [%rd8+1], %r22;
    st.global.u8 [%rd8+2], %r23;

DONE:
    ret;
}
)ptx";

}  // namespace

bool CudaBgrCropScale::Initialize(CudaDriverApi* cuda, std::string* error_out) {
  cuda_ = cuda;
  if (!cuda_) {
    if (error_out) *error_out = "CudaBgrCropScale.Initialize: cuda is null.";
    return false;
  }
  return true;
}

bool CudaBgrCropScale::EnsureKernelLoaded(std::string* error_out) {
  if (loaded_) return true;
  if (!cuda_ || !cuda_->IsInitialized()) {
    if (error_out) *error_out = "CUDA driver API is not initialized.";
    return false;
  }

  const auto& f = cuda_->f();
  CUresult st = f.cuModuleLoadData(&module_, kCropScalePtx);
  if (st != CUDA_SUCCESS) {
    if (error_out) *error_out = "cuModuleLoadData failed: " + cuda_->StatusToString(st);
    return false;
  }

  st = f.cuModuleGetFunction(&fn_, module_, "crop_scale_bgr_u8");
  if (st != CUDA_SUCCESS || !fn_) {
    if (error_out) *error_out = "cuModuleGetFunction(crop_scale_bgr_u8) failed: " + cuda_->StatusToString(st);
    return false;
  }

  loaded_ = true;
  return true;
}

bool CudaBgrCropScale::CropScale(const NvCVImage& src_bgr_gpu,
                                 NvCVImage* dst_bgr_gpu,
                                 float crop_x,
                                 float crop_y,
                                 float crop_w,
                                 float crop_h,
                                 CUstream stream,
                                 std::string* error_out) {
  if (!dst_bgr_gpu) {
    if (error_out) *error_out = "CropScale called with null dst.";
    return false;
  }
  if (!EnsureKernelLoaded(error_out)) return false;

  if (src_bgr_gpu.gpuMem != NVCV_GPU || dst_bgr_gpu->gpuMem != NVCV_GPU) {
    if (error_out) *error_out = "CropScale requires GPU NvCVImage inputs.";
    return false;
  }
  if (src_bgr_gpu.pixelFormat != NVCV_BGR || dst_bgr_gpu->pixelFormat != NVCV_BGR ||
      src_bgr_gpu.componentType != NVCV_U8 || dst_bgr_gpu->componentType != NVCV_U8 ||
      src_bgr_gpu.planar != NVCV_INTERLEAVED || dst_bgr_gpu->planar != NVCV_INTERLEAVED) {
    if (error_out) *error_out = "CropScale expects chunky BGR/U8 NvCVImages.";
    return false;
  }

  const uint32_t srcW = src_bgr_gpu.width;
  const uint32_t srcH = src_bgr_gpu.height;
  const uint32_t dstW = dst_bgr_gpu->width;
  const uint32_t dstH = dst_bgr_gpu->height;

  // Clamp crop rect to valid range.
  crop_w = std::max(1.0f, std::min(crop_w, static_cast<float>(srcW)));
  crop_h = std::max(1.0f, std::min(crop_h, static_cast<float>(srcH)));
  crop_x = std::max(0.0f, std::min(crop_x, static_cast<float>(srcW) - crop_w));
  crop_y = std::max(0.0f, std::min(crop_y, static_cast<float>(srcH) - crop_h));

  const uint64_t srcPtr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(src_bgr_gpu.pixels));
  const uint64_t dstPtr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(dst_bgr_gpu->pixels));
  const uint32_t srcPitch = static_cast<uint32_t>(std::max(0, src_bgr_gpu.pitch));
  const uint32_t dstPitch = static_cast<uint32_t>(std::max(0, dst_bgr_gpu->pitch));

  void* params[] = {
      (void*)&srcPtr,
      (void*)&srcPitch,
      (void*)&srcW,
      (void*)&srcH,
      (void*)&dstPtr,
      (void*)&dstPitch,
      (void*)&dstW,
      (void*)&dstH,
      (void*)&crop_x,
      (void*)&crop_y,
      (void*)&crop_w,
      (void*)&crop_h,
  };

  constexpr unsigned int kBlockX = 16;
  constexpr unsigned int kBlockY = 16;
  const unsigned int gridX = (dstW + kBlockX - 1) / kBlockX;
  const unsigned int gridY = (dstH + kBlockY - 1) / kBlockY;

  const CUresult st = cuda_->f().cuLaunchKernel(fn_,
                                               gridX,
                                               gridY,
                                               1,
                                               kBlockX,
                                               kBlockY,
                                               1,
                                               0,
                                               stream,
                                               params,
                                               nullptr);
  if (st != CUDA_SUCCESS) {
    if (error_out) *error_out = "cuLaunchKernel(crop_scale_bgr_u8) failed: " + cuda_->StatusToString(st);
    return false;
  }

  return true;
}

}  // namespace studiocast::maxine
