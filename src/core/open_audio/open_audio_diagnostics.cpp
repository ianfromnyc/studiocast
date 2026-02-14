#include "core/open_audio/open_audio_diagnostics.h"

#include <sstream>

#include "core/util/json.h"

namespace studiocast::open_audio {

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

void AppendJsonModels(std::ostringstream* oss, const std::vector<OpenAudioDiagnostics::ModelInfo>& models) {
  *oss << "[";
  for (std::size_t i = 0; i < models.size(); ++i) {
    if (i) *oss << ",";
    const auto& m = models[i];
    *oss << "{";
    *oss << "\"id\":\"" << JsonEscape(m.id) << "\",";
    *oss << "\"display_name\":\"" << JsonEscape(m.display_name) << "\",";
    *oss << "\"effects\":";
    AppendJsonStringArray(oss, m.effects);
    *oss << ",";
    *oss << "\"sample_rate\":" << m.sample_rate << ",";
    *oss << "\"channels\":" << m.channels;
    *oss << "}";
  }
  *oss << "]";
}

}  // namespace

std::string OpenAudioDiagnostics::ToJson() const {
  std::ostringstream oss;
  oss << "{";

  oss << "\"ok\":" << BoolJson(ok) << ",";

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

}  // namespace studiocast::open_audio
