#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "core/open_video/fastdvdnet_denoiser.h"

namespace {

bool Require(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << message << "\n";
    return false;
  }
  return true;
}

bool RequireShape(const std::vector<int64_t> &got,
                  const std::vector<int64_t> &want,
                  const std::string &message) {
  if (got == want)
    return true;
  std::cerr << message << "\n";
  return false;
}

} // namespace

namespace studiocast::tests {

bool TestFastDvdnetDenoiseTensorContractIsDeclared() {
  studiocast::open_video::FastDvdnetDenoiser denoiser;
  const auto &contract = denoiser.tensor_io_contract();

  bool ok = true;
  ok &= Require(contract.adapter_id == "fastdvdnet",
                "unexpected denoise adapter id");
  ok &= Require(contract.model_family == "FastDVDnet",
                "unexpected denoise model family");
  ok &= Require(contract.inputs.size() == 2,
                "FastDVDnet contract should declare two inputs");

  if (contract.inputs.size() == 2) {
    ok &= Require(contract.inputs[0].role == "temporal_rgb_window",
                  "unexpected temporal input role");
    ok &= Require(contract.inputs[0].layout == "NCHW",
                  "unexpected temporal input layout");
    ok &= RequireShape(contract.inputs[0].shape, {1, 15, -1, -1},
                       "unexpected temporal input shape");

    ok &= Require(contract.inputs[1].role == "noise_map",
                  "unexpected noise input role");
    ok &= RequireShape(contract.inputs[1].shape, {1, 1, -1, -1},
                       "unexpected noise input shape");
  }

  ok &=
      Require(contract.output.role == "denoised_rgb", "unexpected output role");
  ok &= RequireShape(contract.output.shape, {1, 3, -1, -1},
                     "unexpected output shape");

  ok &= Require(contract.temporal.window_frames == 5,
                "unexpected temporal window");
  ok &= Require(contract.temporal.history_frames == 3,
                "unexpected temporal history");
  ok &= Require(contract.temporal.repeated_future_frames == 2,
                "unexpected repeated future frame count");
  ok &= Require(contract.temporal.causal, "contract should be causal");

  ok &= Require(contract.supports_cpu_tensor_io,
                "contract should support CPU tensor IO");
  ok &= Require(contract.supports_cuda_device_tensor_io,
                "contract should support CUDA device tensor IO");
  ok &= Require(contract.requires_cpu_preprocess,
                "contract should declare CPU preprocess");
  ok &= Require(contract.requires_cpu_postprocess,
                "contract should declare CPU postprocess");
  ok &= Require(contract.requires_output_device_to_cpu_for_postprocess,
                "contract should declare denoised tensor readback");
  return ok;
}

} // namespace studiocast::tests
