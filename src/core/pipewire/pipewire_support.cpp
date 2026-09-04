#include "core/pipewire/pipewire_support.h"

#include <algorithm>
#include <cctype>

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
