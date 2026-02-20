#include "pactl.h"

#include <cstdlib>
#include <sstream>

#include "core/util/exec.h"
#include "core/util/strings.h"

namespace studiocast::audio::pulse {
namespace {
std::vector<std::string> SplitTabs(const std::string &line) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : line) {
    if (c == '\t') {
      out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  out.push_back(cur);
  for (auto &s : out)
    s = util::TrimCopy(s);
  return out;
}

std::string FirstLineOrEmpty(const std::string &s) {
  return util::FirstNonEmptyLine(s);
}
} // namespace

std::optional<std::string>
ParseDefaultFromPactlInfo(const std::string &pactl_info_text,
                          const std::string &key) {
  if (key.empty())
    return std::nullopt;

  for (const auto &raw : util::SplitLines(pactl_info_text)) {
    const auto line = util::TrimCopy(raw);
    if (line.rfind(key, 0) == 0) {
      auto val = util::TrimCopy(line.substr(key.size()));
      if (!val.empty())
        return val;
    }
  }
  return std::nullopt;
}

bool PactlAvailable(std::string *details) {
  auto res = util::ExecCapture("pactl --version 2>&1");
  if (res.exit_code != 0) {
    if (details)
      *details = util::TrimCopy(res.stdout_str);
    return false;
  }
  if (details)
    *details = util::TrimCopy(res.stdout_str);
  return true;
}

std::optional<int> LoadModule(const std::string &module,
                              const std::string &args, std::string *error) {
  // Use stderr->stdout so errors are captured.
  std::string cmd = "pactl load-module " + module;
  if (!args.empty())
    cmd += " " + args;
  cmd += " 2>&1";

  auto res = util::ExecCapture(cmd);
  const std::string line = util::TrimCopy(FirstLineOrEmpty(res.stdout_str));

  if (res.exit_code != 0) {
    if (error)
      *error = line.empty() ? "pactl load-module failed" : line;
    return std::nullopt;
  }

  // pactl prints the module id as a number.
  if (line.empty()) {
    if (error)
      *error = "pactl load-module returned no module id";
    return std::nullopt;
  }

  char *end = nullptr;
  const long id = std::strtol(line.c_str(), &end, 10);
  if (!end || end == line.c_str()) {
    if (error)
      *error = "Failed to parse module id from pactl output: " + line;
    return std::nullopt;
  }

  return static_cast<int>(id);
}

bool UnloadModule(int id, std::string *error) {
  std::ostringstream oss;
  oss << "pactl unload-module " << id << " 2>&1";
  auto res = util::ExecCapture(oss.str());
  if (res.exit_code != 0) {
    if (error)
      *error = util::TrimCopy(res.stdout_str);
    return false;
  }
  return true;
}

std::vector<PactlModule> ListModules(std::string *error) {
  auto res = util::ExecCapture("pactl list short modules 2>&1");
  if (res.exit_code != 0) {
    if (error)
      *error = util::TrimCopy(res.stdout_str);
    return {};
  }

  std::vector<PactlModule> out;
  for (const auto &raw : util::SplitLines(res.stdout_str)) {
    const auto line = util::TrimCopy(raw);
    if (line.empty())
      continue;

    auto fields = SplitTabs(line);
    if (fields.size() < 2)
      continue;

    PactlModule m;
    m.id = std::atoi(fields[0].c_str());
    m.name = fields[1];
    if (fields.size() >= 3)
      m.args = fields[2];
    out.push_back(m);
  }
  return out;
}

std::vector<PactlSource> ListSources(std::string *error) {
  auto res = util::ExecCapture("pactl list short sources 2>&1");
  if (res.exit_code != 0) {
    if (error)
      *error = util::TrimCopy(res.stdout_str);
    return {};
  }

  std::vector<PactlSource> out;
  for (const auto &raw : util::SplitLines(res.stdout_str)) {
    const auto line = util::TrimCopy(raw);
    if (line.empty())
      continue;

    auto fields = SplitTabs(line);
    if (fields.size() < 2)
      continue;

    PactlSource s;
    s.id = std::atoi(fields[0].c_str());
    s.name = fields[1];
    out.push_back(s);
  }

  return out;
}

std::vector<PactlSink> ListSinks(std::string *error) {
  auto res = util::ExecCapture("pactl list short sinks 2>&1");
  if (res.exit_code != 0) {
    if (error)
      *error = util::TrimCopy(res.stdout_str);
    return {};
  }

  std::vector<PactlSink> out;
  for (const auto &raw : util::SplitLines(res.stdout_str)) {
    const auto line = util::TrimCopy(raw);
    if (line.empty())
      continue;

    auto fields = SplitTabs(line);
    if (fields.size() < 2)
      continue;

    PactlSink s;
    s.id = std::atoi(fields[0].c_str());
    s.name = fields[1];
    out.push_back(s);
  }

  return out;
}

static bool StartsWith(const std::string &s, const std::string &prefix) {
  return s.rfind(prefix, 0) == 0;
}

std::vector<PactlSourceInfo> ListSourcesDetailed(std::string *error) {
  auto res = util::ExecCapture("pactl list sources 2>&1");
  if (res.exit_code != 0) {
    if (error)
      *error = util::TrimCopy(res.stdout_str);
    return {};
  }

  std::vector<PactlSourceInfo> out;
  PactlSourceInfo cur;
  bool haveCur = false;
  bool inPorts = false;

  auto flush = [&]() {
    if (haveCur && !cur.name.empty())
      out.push_back(cur);
    cur = PactlSourceInfo{};
    haveCur = false;
    inPorts = false;
  };

  for (const auto &raw : util::SplitLines(res.stdout_str)) {
    const auto t = util::TrimCopy(raw);
    if (t.empty())
      continue;

    if (StartsWith(t, "Source #")) {
      flush();
      haveCur = true;
      // Parse id after '#'
      const auto pos = t.find('#');
      if (pos != std::string::npos) {
        const auto idStr = util::TrimCopy(t.substr(pos + 1));
        cur.id = std::atoi(idStr.c_str());
      }
      continue;
    }

    if (!haveCur)
      continue;

    // Ports section header
    if (t == "Ports:") {
      inPorts = true;
      continue;
    }

    // Active Port line ends ports parsing too
    if (StartsWith(t, "Active Port:")) {
      cur.active_port =
          util::TrimCopy(t.substr(std::string("Active Port:").size()));
      inPorts = false;
      continue;
    }

    // Stop ports parsing if we hit another top-level-ish key
    if (inPorts) {
      const auto colon = t.find(':');
      if (colon == std::string::npos) {
        inPorts = false;
      } else {
        PactlPort p;
        p.name = util::TrimCopy(t.substr(0, colon));
        auto rest = util::TrimCopy(t.substr(colon + 1));

        // description up to " ("
        auto paren = rest.find(" (");
        if (paren != std::string::npos) {
          p.description = util::TrimCopy(rest.substr(0, paren));
          const auto meta = rest.substr(paren);
          if (meta.find("not available") != std::string::npos)
            p.available = false;
        } else {
          p.description = rest;
        }

        if (!p.name.empty())
          cur.ports.push_back(p);
        continue;
      }
    }

    if (StartsWith(t, "Name:")) {
      cur.name = util::TrimCopy(t.substr(std::string("Name:").size()));
      continue;
    }

    if (StartsWith(t, "Description:")) {
      cur.description =
          util::TrimCopy(t.substr(std::string("Description:").size()));
      continue;
    }
  }

  flush();
  return out;
}

bool SetSourcePort(const std::string &source_name, const std::string &port_name,
                   std::string *error) {
  if (source_name.empty() || port_name.empty())
    return true;

  std::string cmd =
      "pactl set-source-port " + source_name + " " + port_name + " 2>&1";
  auto res = util::ExecCapture(cmd);
  if (res.exit_code != 0) {
    if (error)
      *error = util::TrimCopy(res.stdout_str);
    return false;
  }
  return true;
}

std::optional<std::string> GetDefaultSourceName(std::string *error) {
  // Newer pactl
  auto res = util::ExecCapture("pactl get-default-source 2>&1");
  if (res.exit_code == 0) {
    const auto line = util::TrimCopy(FirstLineOrEmpty(res.stdout_str));
    if (!line.empty())
      return line;
  }

  // Fallback: parse `pactl info`
  res = util::ExecCapture("pactl info 2>&1");
  if (res.exit_code != 0) {
    if (error)
      *error = util::TrimCopy(res.stdout_str);
    return std::nullopt;
  }

  if (auto v = ParseDefaultFromPactlInfo(res.stdout_str, "Default Source:")) {
    return v;
  }

  if (error)
    *error = "Could not determine default source";
  return std::nullopt;
}

std::optional<std::string> GetDefaultSinkName(std::string *error) {
  // Newer pactl
  auto res = util::ExecCapture("pactl get-default-sink 2>&1");
  if (res.exit_code == 0) {
    const auto line = util::TrimCopy(FirstLineOrEmpty(res.stdout_str));
    if (!line.empty())
      return line;
  }

  // Fallback: parse `pactl info`
  res = util::ExecCapture("pactl info 2>&1");
  if (res.exit_code != 0) {
    if (error)
      *error = util::TrimCopy(res.stdout_str);
    return std::nullopt;
  }

  if (auto v = ParseDefaultFromPactlInfo(res.stdout_str, "Default Sink:")) {
    return v;
  }

  if (error)
    *error = "Could not determine default sink";
  return std::nullopt;
}

static std::string ShellQuoteSingle(const std::string &s) {
  // Safe single-quote for /bin/sh -c
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('\'');
  for (char c : s) {
    if (c == '\'') {
      out += "'\"'\"'"; // close, insert escaped quote, reopen
    } else {
      out.push_back(c);
    }
  }
  out.push_back('\'');
  return out;
}

static bool LooksLikeFailure(const std::string &out) {
  // pactl commonly prefixes errors with "Failure:"
  return out.find("Failure:") != std::string::npos ||
         out.find("No such entity") != std::string::npos;
}

bool UpdateSinkProplist(const std::string &sink_name_or_index,
                        const std::vector<std::string> &kv_pairs,
                        std::string *error) {
  if (sink_name_or_index.empty() || kv_pairs.empty())
    return true;

  std::string cmd =
      "pactl update-sink-proplist " + ShellQuoteSingle(sink_name_or_index);
  for (const auto &kv : kv_pairs)
    cmd += " " + ShellQuoteSingle(kv);
  cmd += " 2>&1";

  auto res = util::ExecCapture(cmd);
  const auto out = util::TrimCopy(res.stdout_str);

  if (res.exit_code != 0 || LooksLikeFailure(out)) {
    if (error)
      *error = out.empty() ? "update-sink-proplist failed" : out;
    return false;
  }
  return true;
}

bool UpdateSourceProplist(const std::string &source_name_or_index,
                          const std::vector<std::string> &kv_pairs,
                          std::string *error) {
  if (source_name_or_index.empty() || kv_pairs.empty())
    return true;

  std::string cmd =
      "pactl update-source-proplist " + ShellQuoteSingle(source_name_or_index);
  for (const auto &kv : kv_pairs)
    cmd += " " + ShellQuoteSingle(kv);
  cmd += " 2>&1";

  auto res = util::ExecCapture(cmd);
  const auto out = util::TrimCopy(res.stdout_str);

  if (res.exit_code != 0 || LooksLikeFailure(out)) {
    if (error)
      *error = out.empty() ? "update-source-proplist failed" : out;
    return false;
  }
  return true;
}
} // namespace studiocast::audio::pulse
