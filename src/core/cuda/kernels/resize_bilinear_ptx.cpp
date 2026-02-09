#include "core/cuda/kernels/resize_bilinear.h"

#include <cstdint>
#include <limits>
#include <mutex>

namespace studiocast::cuda::kernels {
namespace {

// Bilinear resize kernel (interleaved u8x3, pitch-aware).
// Parameters:
//   srcPtr, srcPitch, srcW, srcH,
//   dstPtr, dstPitch, dstW, dstH
static constexpr const char* kResizeBilinearPtx = R"ptx(
.version 6.0
.target sm_30
.address_size 64

.visible .entry resize_bilinear_u8x3(
    .param .u64 srcPtr,
    .param .u32 srcPitch,
    .param .u32 srcW,
    .param .u32 srcH,
    .param .u64 dstPtr,
    .param .u32 dstPitch,
    .param .u32 dstW,
    .param .u32 dstH
)
{
    .reg .pred  %p<6>;
    .reg .b32   %r<64>;
    .reg .b64   %rd<24>;
    .reg .f32   %f<64>;

    ld.param.u64 %rd1, [srcPtr];
    ld.param.u32 %r1, [srcPitch];
    ld.param.u32 %r2, [srcW];
    ld.param.u32 %r3, [srcH];
    ld.param.u64 %rd2, [dstPtr];
    ld.param.u32 %r4, [dstPitch];
    ld.param.u32 %r5, [dstW];
    ld.param.u32 %r6, [dstH];

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

    // sx = srcW / dstW
    cvt.rn.f32.u32 %f1, %r2;
    cvt.rn.f32.u32 %f2, %r5;
    div.rn.f32 %f3, %f1, %f2;

    // sy = srcH / dstH
    cvt.rn.f32.u32 %f4, %r3;
    cvt.rn.f32.u32 %f5, %r6;
    div.rn.f32 %f6, %f4, %f5;

    // srcX = (x + 0.5) * sx - 0.5
    cvt.rn.f32.s32 %f7, %r10;
    add.f32 %f7, %f7, 0f3F000000; // 0.5
    mul.f32 %f7, %f7, %f3;
    add.f32 %f7, %f7, 0fBF000000; // -0.5

    // srcY = (y + 0.5) * sy - 0.5
    cvt.rn.f32.s32 %f8, %r14;
    add.f32 %f8, %f8, 0f3F000000; // 0.5
    mul.f32 %f8, %f8, %f6;
    add.f32 %f8, %f8, 0fBF000000; // -0.5

    // x0,y0 = trunc(srcX/srcY), then clamp to [0, srcW-1] / [0, srcH-1]
    add.s32 %r15, %r2, -1;         // srcW-1
    add.s32 %r16, %r3, -1;         // srcH-1
    cvt.rzi.s32.f32 %r17, %f7;
    cvt.rzi.s32.f32 %r18, %f8;
    max.s32 %r17, %r17, 0;
    max.s32 %r18, %r18, 0;
    min.s32 %r17, %r17, %r15;
    min.s32 %r18, %r18, %r16;

    // x1 = min(x0+1, srcW-1), y1 = min(y0+1, srcH-1)
    add.s32 %r19, %r17, 1;
    add.s32 %r20, %r18, 1;
    min.s32 %r19, %r19, %r15;
    min.s32 %r20, %r20, %r16;

    // tx = srcX - float(x0)
    cvt.rn.f32.s32 %f11, %r17;
    sub.f32 %f12, %f7, %f11;
    // ty = srcY - float(y0)
    cvt.rn.f32.s32 %f13, %r18;
    sub.f32 %f14, %f8, %f13;

    // Row base pointers
    mul.wide.s32 %rd3, %r18, %r1;      // y0 * srcPitch
    mul.wide.s32 %rd4, %r20, %r1;      // y1 * srcPitch

    // x offsets in bytes
    mul.lo.s32 %r21, %r17, 3;          // x0*3
    mul.lo.s32 %r22, %r19, 3;          // x1*3
    cvt.u64.u32 %rd5, %r21;
    cvt.u64.u32 %rd6, %r22;

    // src addresses
    add.u64 %rd7, %rd1, %rd3;
    add.u64 %rd8, %rd1, %rd4;

    add.u64 %rd9, %rd7, %rd5;          // p00
    add.u64 %rd10, %rd7, %rd6;         // p10
    add.u64 %rd11, %rd8, %rd5;         // p01
    add.u64 %rd12, %rd8, %rd6;         // p11

    // dst address
    mul.wide.s32 %rd13, %r14, %r4;     // y * dstPitch
    mul.lo.s32 %r23, %r10, 3;          // x*3
    cvt.u64.u32 %rd14, %r23;
    add.u64 %rd15, %rd2, %rd13;
    add.u64 %rd15, %rd15, %rd14;

    // For each channel: v = lerp(lerp(p00,p10,tx), lerp(p01,p11,tx), ty)
    // Channel 0
    ld.global.u8 %r24, [%rd9];
    ld.global.u8 %r25, [%rd10];
    ld.global.u8 %r26, [%rd11];
    ld.global.u8 %r27, [%rd12];
    cvt.rn.f32.u32 %f15, %r24;
    cvt.rn.f32.u32 %f16, %r25;
    cvt.rn.f32.u32 %f17, %r26;
    cvt.rn.f32.u32 %f18, %r27;
    sub.f32 %f19, %f16, %f15;
    mad.rn.f32 %f20, %f12, %f19, %f15;    // v0
    sub.f32 %f21, %f18, %f17;
    mad.rn.f32 %f22, %f12, %f21, %f17;    // v1
    sub.f32 %f23, %f22, %f20;
    mad.rn.f32 %f24, %f14, %f23, %f20;    // v
    add.f32 %f24, %f24, 0f3F000000;    // +0.5
    max.f32 %f24, %f24, 0f00000000;
    min.f32 %f24, %f24, 0f437F0000;    // 255
    cvt.rni.u32.f32 %r28, %f24;
    st.global.u8 [%rd15], %r28;

    // Channel 1
    ld.global.u8 %r29, [%rd9+1];
    ld.global.u8 %r30, [%rd10+1];
    ld.global.u8 %r31, [%rd11+1];
    ld.global.u8 %r32, [%rd12+1];
    cvt.rn.f32.u32 %f25, %r29;
    cvt.rn.f32.u32 %f26, %r30;
    cvt.rn.f32.u32 %f27, %r31;
    cvt.rn.f32.u32 %f28, %r32;
    sub.f32 %f29, %f26, %f25;
    mad.rn.f32 %f30, %f12, %f29, %f25;
    sub.f32 %f31, %f28, %f27;
    mad.rn.f32 %f32, %f12, %f31, %f27;
    sub.f32 %f33, %f32, %f30;
    mad.rn.f32 %f34, %f14, %f33, %f30;
    add.f32 %f34, %f34, 0f3F000000;
    max.f32 %f34, %f34, 0f00000000;
    min.f32 %f34, %f34, 0f437F0000;
    cvt.rni.u32.f32 %r33, %f34;
    st.global.u8 [%rd15+1], %r33;

    // Channel 2
    ld.global.u8 %r34, [%rd9+2];
    ld.global.u8 %r35, [%rd10+2];
    ld.global.u8 %r36, [%rd11+2];
    ld.global.u8 %r37, [%rd12+2];
    cvt.rn.f32.u32 %f35, %r34;
    cvt.rn.f32.u32 %f36, %r35;
    cvt.rn.f32.u32 %f37, %r36;
    cvt.rn.f32.u32 %f38, %r37;
    sub.f32 %f39, %f36, %f35;
    mad.rn.f32 %f40, %f12, %f39, %f35;
    sub.f32 %f41, %f38, %f37;
    mad.rn.f32 %f42, %f12, %f41, %f37;
    sub.f32 %f43, %f42, %f40;
    mad.rn.f32 %f44, %f14, %f43, %f40;
    add.f32 %f44, %f44, 0f3F000000;
    max.f32 %f44, %f44, 0f00000000;
    min.f32 %f44, %f44, 0f437F0000;
    cvt.rni.u32.f32 %r38, %f44;
    st.global.u8 [%rd15+2], %r38;

DONE:
    ret;
}
)ptx";

struct GlobalCuda {
  std::once_flag once;
  studiocast::maxine::CudaDriverApi cuda;
  bool ok = false;
  std::string err;
};

GlobalCuda& g() {
  static GlobalCuda s;
  return s;
}

bool EnsureCudaReady(studiocast::maxine::CudaDriverApi** out_cuda, std::string* error_out) {
  if (error_out) error_out->clear();
  GlobalCuda& st = g();
  std::call_once(st.once, [&]() {
    std::string e;
    if (!st.cuda.Initialize(&e)) {
      st.err = e;
      st.ok = false;
      return;
    }
    st.ok = true;
  });
  if (!st.ok) {
    if (error_out) *error_out = st.err.empty() ? "CUDA unavailable" : st.err;
    return false;
  }
  std::string e;
  if (!st.cuda.EnsureContext(&e)) {
    if (error_out) *error_out = e;
    return false;
  }
  *out_cuda = &st.cuda;
  return true;
}

struct KernelState {
  bool loaded = false;
  studiocast::maxine::CUmodule module = nullptr;
  studiocast::maxine::CUfunction fn = nullptr;
};

KernelState& kernel() {
  static KernelState s;
  return s;
}

bool EnsureKernelLoaded(studiocast::maxine::CudaDriverApi* cuda, std::string* error_out) {
  if (error_out) error_out->clear();
  KernelState& k = kernel();
  if (k.loaded) return true;

  const auto& f = cuda->f();
  studiocast::maxine::CUresult st = studiocast::maxine::CUDA_SUCCESS;
  std::string jit_log;
  if (f.cuModuleLoadDataEx) {
    char info[8192] = {0};
    char err[8192] = {0};

    studiocast::maxine::CUjit_option opts[] = {
        studiocast::maxine::CU_JIT_INFO_LOG_BUFFER,
        studiocast::maxine::CU_JIT_INFO_LOG_BUFFER_SIZE_BYTES,
        studiocast::maxine::CU_JIT_ERROR_LOG_BUFFER,
        studiocast::maxine::CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES,
        studiocast::maxine::CU_JIT_LOG_VERBOSE,
    };
    void* vals[] = {
        info,
        reinterpret_cast<void*>(static_cast<std::size_t>(sizeof(info))),
        err,
        reinterpret_cast<void*>(static_cast<std::size_t>(sizeof(err))),
        reinterpret_cast<void*>(static_cast<std::size_t>(1)),
    };

    st = f.cuModuleLoadDataEx(&k.module,
                             kResizeBilinearPtx,
                             static_cast<unsigned int>(sizeof(opts) / sizeof(opts[0])),
                             opts,
                             vals);
    jit_log = std::string("PTX JIT info log:\n") + info + "\nPTX JIT error log:\n" + err;
  } else {
    st = f.cuModuleLoadData(&k.module, kResizeBilinearPtx);
  }
  if (st != studiocast::maxine::CUDA_SUCCESS) {
    if (error_out) {
      *error_out = "cuModuleLoadData(resize_bilinear_u8x3) failed: " + cuda->StatusToString(st);
      if (!jit_log.empty()) *error_out += "\n" + jit_log;
    }
    return false;
  }
  st = f.cuModuleGetFunction(&k.fn, k.module, "resize_bilinear_u8x3");
  if (st != studiocast::maxine::CUDA_SUCCESS) {
    if (error_out) *error_out = "cuModuleGetFunction(resize_bilinear_u8x3) failed: " + cuda->StatusToString(st);
    return false;
  }
  k.loaded = true;
  return true;
}

}  // namespace

bool ResizeBilinear(const CudaImage& src,
                    const CudaImage& dst,
                    studiocast::maxine::CUstream stream,
                    std::string* error_out) {
  if (error_out) error_out->clear();
  if (!src.Valid() || !dst.Valid()) {
    if (error_out) *error_out = "ResizeBilinear: invalid src/dst image.";
    return false;
  }
  if (src.format != dst.format) {
    if (error_out) *error_out = "ResizeBilinear: src/dst formats must match.";
    return false;
  }
  if (src.format != PixelFormatGpu::rgb_u8 && src.format != PixelFormatGpu::bgr_u8) {
    if (error_out) *error_out = "ResizeBilinear: unsupported format (expected rgb_u8 or bgr_u8).";
    return false;
  }
  if (src.pitch > std::numeric_limits<std::uint32_t>::max() ||
      dst.pitch > std::numeric_limits<std::uint32_t>::max()) {
    if (error_out) *error_out = "ResizeBilinear: pitch too large for PTX kernel ABI.";
    return false;
  }

  studiocast::maxine::CudaDriverApi* cuda = nullptr;
  if (!EnsureCudaReady(&cuda, error_out)) return false;
  if (!EnsureKernelLoaded(cuda, error_out)) return false;

  KernelState& k = kernel();
  const auto& f = cuda->f();

  const std::uint32_t src_pitch = static_cast<std::uint32_t>(src.pitch);
  const std::uint32_t src_w = static_cast<std::uint32_t>(src.w);
  const std::uint32_t src_h = static_cast<std::uint32_t>(src.h);
  const std::uint32_t dst_pitch = static_cast<std::uint32_t>(dst.pitch);
  const std::uint32_t dst_w = static_cast<std::uint32_t>(dst.w);
  const std::uint32_t dst_h = static_cast<std::uint32_t>(dst.h);
  const unsigned long long src_ptr = src.ptr;
  const unsigned long long dst_ptr = dst.ptr;

  void* args[] = {
      const_cast<unsigned long long*>(&src_ptr),
      const_cast<std::uint32_t*>(&src_pitch),
      const_cast<std::uint32_t*>(&src_w),
      const_cast<std::uint32_t*>(&src_h),
      const_cast<unsigned long long*>(&dst_ptr),
      const_cast<std::uint32_t*>(&dst_pitch),
      const_cast<std::uint32_t*>(&dst_w),
      const_cast<std::uint32_t*>(&dst_h),
  };

  constexpr unsigned int block_x = 16;
  constexpr unsigned int block_y = 16;
  const unsigned int grid_x = (static_cast<unsigned int>(dst.w) + block_x - 1u) / block_x;
  const unsigned int grid_y = (static_cast<unsigned int>(dst.h) + block_y - 1u) / block_y;

  const studiocast::maxine::CUresult st = f.cuLaunchKernel(k.fn,
                                                          grid_x,
                                                          grid_y,
                                                          1,
                                                          block_x,
                                                          block_y,
                                                          1,
                                                          0,
                                                          stream,
                                                          args,
                                                          nullptr);
  if (st != studiocast::maxine::CUDA_SUCCESS) {
    if (error_out) *error_out = "cuLaunchKernel(resize_bilinear_u8x3) failed: " + cuda->StatusToString(st);
    return false;
  }
  return true;
}

}  // namespace studiocast::cuda::kernels
