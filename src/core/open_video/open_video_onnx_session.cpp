#include "core/open_video/open_video_onnx_session.h"

#include <utility>

namespace studiocast::open_video {

struct OpenVideoOrtSession::Impl {
  std::unique_ptr<studiocast::onnx::OrtSession> session;
};

OpenVideoOrtSession::OpenVideoOrtSession(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
OpenVideoOrtSession::~OpenVideoOrtSession() = default;

const OrtSessionInfo& OpenVideoOrtSession::info() const { return impl_->session->info(); }

OrtRuntimeInfo OpenVideoOrtSession::QueryRuntimeInfo() {
  return studiocast::onnx::OrtSession::QueryRuntimeInfo();
}

std::unique_ptr<OpenVideoOrtSession> OpenVideoOrtSession::Create(const std::filesystem::path& model_path,
                                                                 const OrtSessionOptions& opts,
                                                                 OrtSessionInfo* info_out,
                                                                 std::string* error) {
  auto session = studiocast::onnx::OrtSession::Create(model_path, opts, info_out, error);
  if (!session) {
    return nullptr;
  }

  auto impl = std::make_unique<Impl>();
  impl->session = std::move(session);
  return std::unique_ptr<OpenVideoOrtSession>(new OpenVideoOrtSession(std::move(impl)));
}

bool OpenVideoOrtSession::Run(const OrtRunInput* inputs,
                             std::size_t input_count,
                             const OrtRunOutput* outputs,
                             std::size_t output_count,
                             std::string* error) {
  if (error) error->clear();

  if (!impl_ || !impl_->session) {
    if (error) *error = "ORT session is not initialized.";
    return false;
  }

  return impl_->session->RunCpu(inputs, input_count, outputs, output_count, error);
}

}  // namespace studiocast::open_video
