#include "core/pipewire/pipewire_support.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>

namespace studiocast::pw {

bool StreamWentDown(StreamState from, StreamState to) {
  // A stream that was already down cannot go down again.
  if (from == StreamState::kError || from == StreamState::kUnconnected)
    return false;
  return to == StreamState::kError || to == StreamState::kUnconnected;
}

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

// Joins the reason to the note. A note is one line, so it must not be a
// newline: the note goes into the status JSON and into a GUI banner.
constexpr const char *kNoteSeparator = " ";

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

std::string_view ToString(VideoOutputPreference p) {
  switch (p) {
  case VideoOutputPreference::kAuto:
    return "auto";
  case VideoOutputPreference::kV4l2Loopback:
    return "v4l2loopback";
  case VideoOutputPreference::kPipeWire:
    return "pipewire";
  case VideoOutputPreference::kBoth:
    return "both";
  }
  return "auto";
}

std::optional<VideoOutputPreference>
ParseVideoOutputPreference(std::string_view s) {
  const std::string v = Normalize(s);
  if (v == "auto")
    return VideoOutputPreference::kAuto;
  if (v == "v4l2loopback" || v == "v4l2")
    return VideoOutputPreference::kV4l2Loopback;
  if (v == "pipewire" || v == "pw")
    return VideoOutputPreference::kPipeWire;
  if (v == "both")
    return VideoOutputPreference::kBoth;
  return std::nullopt;
}

VideoOutputDecision
ResolveVideoOutputBackends(VideoOutputPreference pref,
                           const PipeWireAvailability &avail) {
  VideoOutputDecision out;

  switch (pref) {
  case VideoOutputPreference::kAuto:
  case VideoOutputPreference::kV4l2Loopback:
    out.backends.v4l2loopback = true;
    return out;

  case VideoOutputPreference::kPipeWire:
    // The pipeline negotiates the frame size, rate and pixel format with the
    // loopback device and mirrors the result onto the node, so the node cannot
    // stand alone yet. Report the loopback that really runs.
    out.backends.v4l2loopback = true;
    out.used_fallback = true;
    if (avail.Usable()) {
      out.backends.pipewire = true;
      out.note = "Node-only camera output is not implemented yet; the "
                 "loopback stays the format source, so v4l2loopback runs "
                 "beside the node.";
      return out;
    }
    out.note = "A native PipeWire camera was requested but is unavailable; "
               "using v4l2loopback.";
    if (const auto r = FirstLine(avail.reason); !r.empty())
      out.note += kNoteSeparator + r;
    return out;

  case VideoOutputPreference::kBoth:
    out.backends.v4l2loopback = true;
    if (avail.Usable()) {
      out.backends.pipewire = true;
      return out;
    }
    out.used_fallback = true;
    out.note = "A native PipeWire camera was requested but is unavailable; "
               "using v4l2loopback alone.";
    if (const auto r = FirstLine(avail.reason); !r.empty())
      out.note += kNoteSeparator + r;
    return out;
  }

  out.backends.v4l2loopback = true;
  return out;
}

std::string NodeLatencyProperty(std::uint32_t frame_samples, int sample_rate) {
  return std::to_string(frame_samples) + "/" + std::to_string(sample_rate);
}

std::string NodeRateProperty(int sample_rate) {
  return "1/" + std::to_string(sample_rate);
}

std::size_t AudioFrameBytes(std::uint32_t frame_samples,
                            std::uint32_t channels) {
  return static_cast<std::size_t>(frame_samples) *
         static_cast<std::size_t>(channels) * sizeof(float);
}

std::size_t AudioRingCapacityBytes(std::uint32_t frame_samples,
                                   std::uint32_t channels, int frames) {
  const int held = frames < 2 ? 2 : frames;
  return static_cast<std::size_t>(held) *
         AudioFrameBytes(frame_samples, channels);
}

void NodeLinkCounter::Reset(LinkEnd end) {
  std::lock_guard<std::mutex> lock(mu_);
  end_ = end;
  node_id_ = 0;
  counted_.clear();
  held_.clear();
  consumer_count_.store(0, std::memory_order_relaxed);
}

bool NodeLinkCounter::MatchesLocked(const Link &link) const {
  return end_ == LinkEnd::kOutput ? link.output_node == node_id_
                                  : link.input_node == node_id_;
}

void NodeLinkCounter::OnLinkAdded(std::uint32_t link_id,
                                  std::uint32_t output_node,
                                  std::uint32_t input_node) {
  const Link link{output_node, input_node};
  std::lock_guard<std::mutex> lock(mu_);

  // The node has no id yet, so there is nothing to compare the link against.
  // Hold it until the id arrives. The cap keeps a busy graph from filling
  // memory; a graph that large means the id is long overdue.
  if (node_id_ == 0) {
    if (held_.size() < kMaxHeldLinks)
      held_[link_id] = link;
    return;
  }

  if (!MatchesLocked(link))
    return;
  if (counted_.insert(link_id).second)
    consumer_count_.fetch_add(1, std::memory_order_relaxed);
}

void NodeLinkCounter::OnGlobalRemoved(std::uint32_t global_id) {
  std::lock_guard<std::mutex> lock(mu_);
  held_.erase(global_id);
  if (counted_.erase(global_id) > 0)
    consumer_count_.fetch_sub(1, std::memory_order_relaxed);
}

void NodeLinkCounter::SetNodeId(std::uint32_t node_id) {
  std::lock_guard<std::mutex> lock(mu_);
  if (node_id == 0 || node_id == node_id_)
    return;
  node_id_ = node_id;

  // Judge the links that arrived before the id, so a consumer that connected
  // at once is counted.
  for (const auto &entry : held_) {
    if (!MatchesLocked(entry.second))
      continue;
    if (counted_.insert(entry.first).second)
      consumer_count_.fetch_add(1, std::memory_order_relaxed);
  }
  held_.clear();
}

std::uint32_t NodeLinkCounter::NodeId() const {
  std::lock_guard<std::mutex> lock(mu_);
  return node_id_;
}

PipeWireSocketProbe ProbePipeWireSocket(const PipeWireProbeEnv &env) {
  PipeWireSocketProbe out;
  if (!env.get_env || !env.path_exists) {
    out.reason = "The PipeWire probe has no environment hooks.";
    return out;
  }

  // PipeWire itself reads these three variables, in this order. USERPROFILE
  // is the one its Windows build uses, so on Linux it is almost always empty.
  std::string dir = env.get_env("PIPEWIRE_RUNTIME_DIR");
  if (dir.empty())
    dir = env.get_env("XDG_RUNTIME_DIR");
  if (dir.empty())
    dir = env.get_env("USERPROFILE");
  if (dir.empty()) {
    out.reason = "No PipeWire runtime directory: PIPEWIRE_RUNTIME_DIR, "
                 "XDG_RUNTIME_DIR and USERPROFILE are not set.";
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
      out.note += kNoteSeparator + r;
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
