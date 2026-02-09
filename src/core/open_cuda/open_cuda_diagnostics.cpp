#include "core/open_cuda/open_cuda_diagnostics.h"

#include <sstream>

#include "core/util/json.h"

namespace studiocast::open_cuda {

namespace {

std::string BoolJson(bool v) { return v ? "true" : "false"; }

std::string JsonEscape(const std::string& s) { return studiocast::util::json::EscapeString(s); }

void AppendJsonStringArray(std::ostringstream* oss, const std::vector<std::string>& a) {
  *oss << "[";
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (i) *oss << ",";
    *oss << "\"" << JsonEscape(a[i]) << "\"";
  }
  *oss << "]";
}

void AppendJsonStringMap(std::ostringstream* oss, const std::map<std::string, std::string>& m) {
  *oss << "{";
  bool first = true;
  for (const auto& [k, v] : m) {
    if (!first) *oss << ",";
    first = false;
    *oss << "\"" << JsonEscape(k) << "\":";
    *oss << "\"" << JsonEscape(v) << "\"";
  }
  *oss << "}";
}

}  // namespace

std::string OpenCudaDiagnostics::ToJson() const {
  std::ostringstream oss;
  oss << "{";

  oss << "\"ok\":" << BoolJson(ok) << ",";

  oss << "\"installed_models\":";
  AppendJsonStringArray(&oss, installed_models);
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

}  // namespace studiocast::open_cuda
