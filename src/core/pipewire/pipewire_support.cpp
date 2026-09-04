#include "core/pipewire/pipewire_support.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>

namespace studiocast::pw {

namespace {

// Removes the surrounding spaces and makes the text lower case, so config
// files and command lines can be forgiving.
std::string Normalize(std::string_view s) {
  std::size_t b = 0;
  std::size_t e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
    ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
    --e;
  std::string out(s.substr(b, e - b));
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return out;
}

// Keeps a status note to one line so a GUI banner stays readable.
std::string FirstLine(const std::string &s) {
  const auto pos = s.find('\n');
  if (pos == std::string::npos)
    return s;
  return s.substr(0, pos);
}

} // namespace

std::string_view ToString(AudioTransport t) {
  switch (t) {
  case AudioTransport::kPulse:
    return "pulse";
  case AudioTransport::kPipeWire:
    return "pipewire";
  }
  return "pulse";
}

std::string_view ToString(AudioTransportPreference p) {
  switch (p) {
  case AudioTransportPreference::kAuto:
    return "auto";
  case AudioTransportPreference::kPulse:
    return "pulse";
  case AudioTransportPreference::kPipeWire:
    return "pipewire";
  }
  return "auto";
}

PipeWireSocketProbe ProbePipeWireSocket(const PipeWireProbeEnv &env) {
  PipeWireSocketProbe out;
  if (!env.get_env || !env.path_exists) {
    out.reason = "The PipeWire probe has no environment hooks.";
    return out;
  }

  // PipeWire itself reads these three variables, in this order.
  std::string dir = env.get_env("PIPEWIRE_RUNTIME_DIR");
  if (dir.empty())
    dir = env.get_env("XDG_RUNTIME_DIR");
  if (dir.empty())
    dir = env.get_env("USERPROFILE");
  if (dir.empty()) {
    out.reason = "No PipeWire runtime directory: PIPEWIRE_RUNTIME_DIR and "
                 "XDG_RUNTIME_DIR are not set.";
    return out;
  }

  std::string name = env.get_env("PIPEWIRE_REMOTE");
  if (name.empty())
    name = "pipewire-0";

  while (!dir.empty() && dir.back() == '/')
    dir.pop_back();
  out.path = dir + "/" + name;

  if (!env.path_exists(out.path)) {
    out.reason = "PipeWire socket " + out.path + " was not found.";
    return out;
  }

  out.found = true;
  return out;
}

PipeWireSocketProbe ProbePipeWireSocket() {
  PipeWireProbeEnv env;
  env.get_env = [](const char *name) -> std::string {
    const char *v = name ? std::getenv(name) : nullptr;
    return v ? std::string(v) : std::string();
  };
  env.path_exists = [](const std::string &path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
  };
  return ProbePipeWireSocket(env);
}

PipeWireAvailability ProbePipeWire() {
  PipeWireAvailability out;
  out.compiled_in = PipeWireCompiledIn();
  if (!out.compiled_in) {
    out.reason = "StudioCast was built without PipeWire support "
                 "(STUDIOCAST_ENABLE_PIPEWIRE=OFF).";
    return out;
  }

  const auto socket = ProbePipeWireSocket();
  out.server_reachable = socket.found;
  out.reason = socket.reason;
  return out;
}

std::optional<AudioTransportPreference>
ParseAudioTransportPreference(std::string_view s) {
  const std::string v = Normalize(s);
  if (v == "auto")
    return AudioTransportPreference::kAuto;
  if (v == "pulse" || v == "pulseaudio")
    return AudioTransportPreference::kPulse;
  if (v == "pipewire" || v == "pw")
    return AudioTransportPreference::kPipeWire;
  return std::nullopt;
}

AudioTransportDecision
ResolveAudioTransport(AudioTransportPreference pref,
                      const PipeWireAvailability &avail) {
  AudioTransportDecision out;

  switch (pref) {
  case AudioTransportPreference::kPulse:
    out.transport = AudioTransport::kPulse;
    return out;

  case AudioTransportPreference::kPipeWire:
    if (avail.Usable()) {
      out.transport = AudioTransport::kPipeWire;
      return out;
    }
    out.transport = AudioTransport::kPulse;
    out.used_fallback = true;
    out.note =
        "Native PipeWire audio requested but unavailable; using PulseAudio.";
    if (const auto r = FirstLine(avail.reason); !r.empty())
      out.note += "\n" + r;
    return out;

  case AudioTransportPreference::kAuto:
    // PulseAudio is the documented default, so falling back to it is not a
    // degradation and does not set a note.
    out.transport =
        avail.Usable() ? AudioTransport::kPipeWire : AudioTransport::kPulse;
    return out;
  }

  out.transport = AudioTransport::kPulse;
  return out;
}

} // namespace studiocast::pw
