#include "core/open_video/diagnostics.h"

#include <sstream>

#include "core/util/json.h"

namespace studiocast::open_cuda {

namespace {

std::string BoolJson(bool v) { return v ? "true" : "false"; }

std::string JsonEscape(const std::string &s) {
  return studiocast::util::json::EscapeString(s);
}

void AppendJsonStringArray(std::ostringstream *oss,
                           const std::vector<std::string> &a) {
  *oss << "[";
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (i)
      *oss << ",";
    *oss << "\"" << JsonEscape(a[i]) << "\"";
  }
  *oss << "]";
}

void AppendJsonStringMap(std::ostringstream *oss,
                         const std::map<std::string, std::string> &m) {
  *oss << "{";
  bool first = true;
  for (const auto &[k, v] : m) {
    if (!first)
      *oss << ",";
    first = false;
    *oss << "\"" << JsonEscape(k) << "\":";
    *oss << "\"" << JsonEscape(v) << "\"";
  }
  *oss << "}";
}

void AppendJsonModels(
    std::ostringstream *oss,
    const std::vector<OpenCudaDiagnostics::ModelInfo> &models) {
  *oss << "[";
  for (std::size_t i = 0; i < models.size(); ++i) {
    if (i)
      *oss << ",";
    const auto &m = models[i];
    *oss << "{";
    *oss << "\"id\":\"" << JsonEscape(m.id) << "\",";
    *oss << "\"display_name\":\"" << JsonEscape(m.display_name) << "\",";
    *oss << "\"task\":\"" << JsonEscape(m.task) << "\",";
    *oss << "\"width\":" << m.width << ",";
    *oss << "\"height\":" << m.height;
    *oss << "}";
  }
  *oss << "]";
}

} // namespace

std::string OpenCudaDiagnostics::ToJson() const {
  std::ostringstream oss;
  oss << "{";

  oss << "\"ok\":" << BoolJson(ok) << ",";

  oss << "\"onnxruntime_version\":\"" << JsonEscape(onnxruntime_version)
      << "\",";
  oss << "\"onnxruntime_providers\":";
  AppendJsonStringArray(&oss, onnxruntime_providers);
  oss << ",";
  oss << "\"onnxruntime_cuda_provider_present\":"
      << BoolJson(onnxruntime_cuda_provider_present) << ",";
  oss << "\"onnxruntime_tensorrt_provider_present\":"
      << BoolJson(onnxruntime_tensorrt_provider_present) << ",";
  oss << "\"onnxruntime_cpu_provider_present\":"
      << BoolJson(onnxruntime_cpu_provider_present) << ",";
  oss << "\"onnxruntime_cuda_ep_v2_build\":"
      << BoolJson(onnxruntime_cuda_ep_v2_build) << ",";
  oss << "\"onnxruntime_library_path\":\""
      << JsonEscape(onnxruntime_library_path) << "\",";
  oss << "\"onnxruntime_warnings\":";
  AppendJsonStringArray(&oss, onnxruntime_warnings);
  oss << ",";

  oss << "\"cuda_driver_api_available\":"
      << BoolJson(cuda_driver_api_available) << ",";
  oss << "\"cuda_context_available\":"
      << BoolJson(cuda_context_available) << ",";
  oss << "\"cuda_device_count\":" << cuda_device_count << ",";
  oss << "\"cuda_driver_version\":" << cuda_driver_version << ",";
  oss << "\"cuda_driver_error\":\"" << JsonEscape(cuda_driver_error) << "\",";
  oss << "\"cuda_context_error\":\"" << JsonEscape(cuda_context_error)
      << "\",";

  oss << "\"tensorrt_supported\":" << BoolJson(tensorrt_supported) << ",";

  oss << "\"tensorrt_available\":" << BoolJson(tensorrt_available) << ",";

  oss << "\"tensorrt_requested\":" << BoolJson(tensorrt_requested) << ",";

  oss << "\"tensorrt_cache_path\":\"" << JsonEscape(tensorrt_cache_path)
      << "\",";

  oss << "\"tensorrt_status\":\"" << JsonEscape(tensorrt_status) << "\",";

  oss << "\"installed_models\":";
  AppendJsonStringArray(&oss, installed_models);
  oss << ",";

  oss << "\"default_model_id\":\"" << JsonEscape(default_model_id) << "\",";

  oss << "\"models\":";
  AppendJsonModels(&oss, models);
  oss << ",";

  oss << "\"missing_models\":";
  AppendJsonStringMap(&oss, missing_models);
  oss << ",";

  oss << "\"available_effects\":";
  AppendJsonStringArray(&oss, available_effects);
  oss << ",";

  oss << "\"blocked_effects\":";
  AppendJsonStringMap(&oss, blocked_effects);
  oss << ",";

  oss << "\"install_hints\":";
  AppendJsonStringArray(&oss, install_hints);

  oss << "}";
  return oss.str();
}

} // namespace studiocast::open_cuda
