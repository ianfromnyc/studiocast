#include "virtual_mic_state.h"

#include <filesystem>
#include <fstream>
#include <map>

#include "core/util/fs.h"
#include "core/util/strings.h"
#include "core/util/xdg.h"

namespace fs = std::filesystem;

namespace studiocast::audio {
namespace {

std::map<std::string, std::string>
ParseKeyValueFile(const std::string &content) {
  std::map<std::string, std::string> kv;

  for (const auto &lineRaw : util::SplitLines(content)) {
    auto line = util::TrimCopy(lineRaw);
    if (line.empty())
      continue;
    if (line[0] == '#')
      continue;

    const auto pos = line.find('=');
    if (pos == std::string::npos)
      continue;

    auto key = util::TrimCopy(line.substr(0, pos));
    auto val = util::TrimCopy(line.substr(pos + 1));
    if (!key.empty())
      kv[key] = val;
  }

  return kv;
}

std::optional<int> GetOptInt(const std::map<std::string, std::string> &kv,
                             const char *key) {
  auto it = kv.find(key);
  if (it == kv.end())
    return std::nullopt;

  const auto v = util::TrimCopy(it->second);
  if (v.empty())
    return std::nullopt;
  return std::atoi(v.c_str());
}

} // namespace

fs::path VirtualMicStatePath() {
  const auto dir = util::StudioCastStateDir();
  if (dir.empty())
    return {};
  return dir / "audio.conf";
}

VirtualMicState LoadVirtualMicState() {
  VirtualMicState s;

  const auto path = VirtualMicStatePath();
  if (path.empty())
    return s;

  auto content = util::ReadTextFile(path.string());
  if (!content)
    return s;

  const auto kv = ParseKeyValueFile(*content);
  s.null_sink_module_id = GetOptInt(kv, "null_sink_module_id");
  s.remap_source_module_id = GetOptInt(kv, "remap_source_module_id");
  s.loopback_module_id = GetOptInt(kv, "loopback_module_id");
  return s;
}

bool SaveVirtualMicState(const VirtualMicState &s, std::string *error) {
  const auto path = VirtualMicStatePath();
  if (path.empty()) {
    if (error)
      *error = "VirtualMicStatePath() is empty (HOME/XDG_STATE_HOME missing)";
    return false;
  }

  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (ec) {
    if (error)
      *error = "Failed to create state dir: " + ec.message();
    return false;
  }

  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out.is_open()) {
    if (error)
      *error = "Failed to write state file: " + path.string();
    return false;
  }

  out << "# StudioCast audio runtime state (module ids)\n";
  if (s.null_sink_module_id)
    out << "null_sink_module_id=" << *s.null_sink_module_id << "\n";
  if (s.remap_source_module_id)
    out << "remap_source_module_id=" << *s.remap_source_module_id << "\n";
  if (s.loopback_module_id)
    out << "loopback_module_id=" << *s.loopback_module_id << "\n";

  return true;
}

bool ClearVirtualMicState(std::string *error) {
  const auto path = VirtualMicStatePath();
  if (path.empty())
    return true;

  std::error_code ec;
  fs::remove(path, ec);
  if (ec) {
    if (error)
      *error = "Failed to remove state file: " + ec.message();
    return false;
  }
  return true;
}

} // namespace studiocast::audio
