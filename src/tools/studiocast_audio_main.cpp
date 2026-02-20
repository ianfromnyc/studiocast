#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "core/audio/audio_pipeline.h"
#include "core/audio/pulse/pactl.h"
#include "core/audio/virtual_mic.h"
#include "core/audio/virtual_speaker.h"
#include "core/config/settings.h"
#include "core/maxine/afx/afx_audio_processor.h"
#include "core/maxine/afx/afx_effect.h"
#include "core/maxine/afx_api.h"
#include "core/maxine/gpu_selection.h"
#include "core/maxine/paths.h"
#include "core/util/strings.h"

namespace {

[[maybe_unused]] bool LooksLikeFeedbackLoopSource(const std::string &name) {
  if (name == "studiocast_mic")
    return true;
  if (name.find(".monitor") != std::string::npos)
    return true;
  return false;
}

void Usage(const char *argv0) {
  std::cout
      << "StudioCast Audio Tool\n\n"
      << "Usage:\n"
      << "  " << argv0 << " status\n"
      << "  " << argv0 << " create\n"
      << "  " << argv0 << " destroy\n"
      << "  " << argv0 << " speakers-create\n"
      << "  " << argv0 << " speakers-destroy\n"
      << "  " << argv0
      << " speakers-loopback-start [--sink <name>] [--latency-ms <n>]\n"
      << "  " << argv0 << " speakers-loopback-stop\n"
      << "  " << argv0
      << " loopback-start [--source <name>] [--latency-ms <n>]  (debug-only)\n"
      << "  " << argv0 << " loopback-stop\n"
      << "  " << argv0
      << " pipeline-run [--source <name>] [--strength <0..100>] [--noise] "
         "[--echo] [--studio-voice]\n"
      << "               [--denoiser-v2] [--duration-sec <n>] "
         "[--status-interval-ms <n>]\n"
      << "  " << argv0
      << " speakers-denoise-run [--sink <name>] [--strength <0..100>] "
         "[--denoiser-v2]\n"
      << "               [--duration-sec <n>] [--status-interval-ms <n>]\n";
}

[[maybe_unused]] bool HasArg(int argc, char **argv, std::string_view flag) {
  for (int i = 1; i < argc; ++i) {
    if (argv[i] && std::string_view(argv[i]) == flag)
      return true;
  }
  return false;
}

std::string GetArgValue(int argc, char **argv, std::string_view key) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (argv[i] && std::string_view(argv[i]) == key) {
      return argv[i + 1] ? std::string(argv[i + 1]) : "";
    }
  }
  return "";
}

int GetArgInt(int argc, char **argv, std::string_view key, int fallback) {
  const auto v = GetArgValue(argc, argv, key);
  if (v.empty())
    return fallback;
  return std::atoi(v.c_str());
}

std::atomic<bool> g_stop{false};

extern "C" void OnSignal(int /*signum*/) {
  g_stop.store(true, std::memory_order_release);
}

std::optional<std::string> ChooseDefaultPhysicalSink(std::string *error) {
  auto def = studiocast::audio::pulse::GetDefaultSinkName(error);
  if (def && *def != "studiocast_speakers" && *def != "studiocast_sink") {
    return def;
  }

  const auto sinks = studiocast::audio::pulse::ListSinks(error);
  for (const auto &s : sinks) {
    if (s.name == "studiocast_speakers")
      continue;
    if (s.name == "studiocast_sink")
      continue;
    return s.name;
  }

  if (error && error->empty())
    *error = "No sinks found";
  return std::nullopt;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    Usage(argv[0]);
    return 1;
  }

  const std::string cmd = argv[1];

  if (cmd == "status") {
    std::cout << studiocast::audio::StatusText() << "\n";
    return 0;
  }

  if (cmd == "create") {
    std::string err;
    if (!studiocast::audio::CreateVirtualMic(&err)) {
      std::cerr << "ERROR: " << err << "\n";
      return 2;
    }
    std::cout << "Created StudioCast virtual mic.\n";
    return 0;
  }

  if (cmd == "destroy") {
    std::string err;
    if (!studiocast::audio::DestroyVirtualMic(&err)) {
      std::cerr << "ERROR: " << err << "\n";
      return 2;
    }
    std::cout << "Destroyed StudioCast virtual mic.\n";
    return 0;
  }

  if (cmd == "speakers-create") {
    std::string err;
    if (!studiocast::audio::CreateVirtualSpeaker(&err)) {
      std::cerr << "ERROR: " << err << "\n";
      return 2;
    }
    std::cout << "Created StudioCast Speakers.\n";
    return 0;
  }

  if (cmd == "speakers-destroy") {
    std::string err;
    if (!studiocast::audio::DestroyVirtualSpeaker(&err)) {
      std::cerr << "ERROR: " << err << "\n";
      return 2;
    }
    std::cout << "Destroyed StudioCast Speakers.\n";
    return 0;
  }

  if (cmd == "speakers-loopback-start") {
    const std::string sink = GetArgValue(argc, argv, "--sink");
    const int latency = GetArgInt(argc, argv, "--latency-ms", 10);

    std::string chosen = sink;
    if (chosen.empty()) {
      std::string err;
      const auto s = ChooseDefaultPhysicalSink(&err);
      if (!s) {
        std::cerr
            << "ERROR: Failed to choose a physical sink: " << err << "\n"
            << "Tip: pass an explicit --sink (see: studiocast-audio status).\n";
        return 2;
      }
      chosen = *s;
    }

    std::string err;
    if (!studiocast::audio::StartSpeakerLoopback(chosen, latency, &err)) {
      std::cerr << "ERROR: " << err << "\n";
      return 2;
    }

    std::cout
        << "Speaker loopback started (source=studiocast_speakers.monitor, sink="
        << chosen << ", latency_ms=" << latency << ").\n";
    return 0;
  }

  if (cmd == "speakers-loopback-stop") {
    std::string err;
    if (!studiocast::audio::StopSpeakerLoopback(&err)) {
      std::cerr << "ERROR: " << err << "\n";
      return 2;
    }
    std::cout << "Speaker loopback stopped.\n";
    return 0;
  }

  if (cmd == "loopback-start") {
    const std::string source = GetArgValue(argc, argv, "--source");
    const int latency = GetArgInt(argc, argv, "--latency-ms", 10);

    std::string err;
    if (!studiocast::audio::StartLoopback(source, latency, &err)) {
      std::cerr << "ERROR: " << err << "\n";
      return 2;
    }

    std::cout << "Loopback started (source="
              << (source.empty() ? "<default>" : source)
              << ", latency_ms=" << latency << ").\n";
    return 0;
  }

  if (cmd == "loopback-stop") {
    std::string err;
    if (!studiocast::audio::StopLoopback(&err)) {
      std::cerr << "ERROR: " << err << "\n";
      return 2;
    }
    std::cout << "Loopback stopped.\n";
    return 0;
  }

  if (cmd == "pipeline-run") {
#if !STUDIOCAST_HAVE_PULSE_SIMPLE
    std::cerr
        << "ERROR: This build was compiled without libpulse-simple support.\n"
        << "Install libpulse-dev (provides libpulse-simple) and rebuild to "
           "enable the real-time audio pipeline.\n";
    return 2;
#else
    const std::string source = GetArgValue(argc, argv, "--source");
    const int strength = GetArgInt(argc, argv, "--strength", 50);
    const bool noise = HasArg(argc, argv, "--noise");
    const bool echo = HasArg(argc, argv, "--echo");
    const bool studioVoice = HasArg(argc, argv, "--studio-voice");
    const bool denoiserV2 = HasArg(argc, argv, "--denoiser-v2");
    const int durationSec = GetArgInt(argc, argv, "--duration-sec", 0);
    const int statusIntervalMs =
        GetArgInt(argc, argv, "--status-interval-ms", 1000);

    {
      std::string err;
      if (!studiocast::audio::CreateVirtualMic(&err)) {
        std::cerr << "ERROR: " << err << "\n";
        return 2;
      }
      // Avoid double-routing; best-effort.
      studiocast::audio::StopLoopback(&err);
    }

    // Feedback-loop guard: don't let the pipeline capture from our own virtual
    // mic or any monitor source. If the user didn't specify --source, resolve
    // default source name via pactl so we can validate it.
    std::string chosenSource = source;
    if (chosenSource.empty()) {
      std::string derr;
      const auto def = studiocast::audio::pulse::GetDefaultSourceName(&derr);
      if (!def) {
        std::cerr << "ERROR: Failed to query default Pulse source via pactl: "
                  << derr << "\n"
                  << "Please rerun with an explicit --source (a real "
                     "microphone source).\n";
        return 2;
      }
      chosenSource = *def;
    }
    if (LooksLikeFeedbackLoopSource(chosenSource)) {
      std::cerr
          << "ERROR: Refusing to capture from '" << chosenSource
          << "' to avoid a feedback loop.\n"
          << "Pick a real microphone source (try: studiocast-audio status).\n";
      return 2;
    }

    const auto settings = studiocast::config::LoadSettings();
    const auto sel = studiocast::maxine::SelectGpu(settings.gpu);
    if (!sel.selected || !sel.selected->compute_capability) {
      std::cerr << "ERROR: Failed to select a supported NVIDIA GPU. "
                << sel.error << "\n";
      return 2;
    }
    std::cout << "Selected GPU: index=" << sel.selected->index << ", name='"
              << sel.selected->name
              << "', compute_cap=" << sel.selected->ComputeCapString() << "\n";

    const auto paths = studiocast::maxine::ResolveMaxinePaths();
    if (!paths.afx.ok) {
      std::cerr << "ERROR: AFX SDK not available.\n";
      for (const auto &p : paths.afx.problems) {
        std::cerr << "  - " << p << "\n";
      }
      return 2;
    }

    studiocast::maxine::afx::AfxApi api;
    {
      std::string err;
      if (!api.InitializeFromLibraryPath(paths.afx.library, &err)) {
        std::cerr << "ERROR: Failed to initialize AFX runtime: " << err << "\n";
        return 2;
      }
    }

    auto plan = studiocast::maxine::afx::PlanBroadcastMicrophoneEffect(
        studioVoice, noise, echo, strength);
    if (denoiserV2) {
      plan.use_denoiser_v2_model = true;
    }
    if (!plan.enabled) {
      std::cerr << "ERROR: No AFX effect enabled. Use --noise and/or --echo or "
                   "--studio-voice.\n";
      return 2;
    }

    studiocast::maxine::afx::AfxEffect fx(&api);
    {
      studiocast::maxine::afx::AfxEffectConfig cfg;
      cfg.effect_selector = plan.effect_selector;
      cfg.feature_id = plan.feature_id;
      cfg.features_dir = paths.afx.features_dir;
      cfg.compute_capability = sel.selected->compute_capability;
      cfg.sample_rate = 48000;
      cfg.frame_samples = 480;
      cfg.channels = 1;
      cfg.intensity = plan.intensity;
      cfg.use_denoiser_v2_model = plan.use_denoiser_v2_model;

      std::string err;
      if (!fx.Configure(cfg, &err)) {
        std::cerr << "ERROR: Failed to configure AFX effect: " << err << "\n";
        return 2;
      }
      if (!fx.Load(&err)) {
        std::cerr << "ERROR: Failed to load AFX effect: " << err << "\n";
        return 2;
      }
    }

    studiocast::maxine::afx::AfxAudioProcessor processor(&fx);
    studiocast::audio::AudioPipeline pipeline(&processor);
    {
      studiocast::audio::AudioPipelineConfig cfg;
      cfg.source_name = chosenSource;
      cfg.sink_name = "studiocast_sink";

      std::string err;
      if (!pipeline.Start(cfg, &err)) {
        std::cerr << "ERROR: Failed to start audio pipeline: " << err << "\n";
        return 2;
      }
    }

    g_stop.store(false, std::memory_order_release);
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    std::cout << "Pipeline running: source=" << chosenSource
              << " -> sink=studiocast_sink. Press Ctrl+C to stop.";
    if (durationSec > 0) {
      std::cout << " (auto-stop after " << durationSec << "s)";
    }
    std::cout << "\n";

    const auto start = std::chrono::steady_clock::now();
    while (!g_stop.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(statusIntervalMs));

      const auto s = pipeline.GetStats();
      if (!s.last_error.empty()) {
        std::cerr << "ERROR: " << s.last_error << "\n";
        break;
      }

      std::cout << "frames_processed=" << s.frames_processed << "\n";

      if (durationSec > 0) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start);
        if (elapsed.count() >= durationSec) {
          break;
        }
      }
    }

    pipeline.Stop();
    const auto finalStats = pipeline.GetStats();
    if (!finalStats.last_error.empty()) {
      std::cerr << "ERROR: " << finalStats.last_error << "\n";
      return 2;
    }

    std::cout << "Pipeline stopped. frames_processed="
              << finalStats.frames_processed << "\n";
    return 0;
#endif
  }

  if (cmd == "speakers-denoise-run") {
#if !STUDIOCAST_HAVE_PULSE_SIMPLE
    std::cerr
        << "ERROR: This build was compiled without libpulse-simple support.\n"
        << "Install libpulse-dev (provides libpulse-simple) and rebuild to "
           "enable the real-time audio pipeline.\n";
    return 2;
#else
    const int strength = GetArgInt(argc, argv, "--strength", 50);
    const bool denoiserV2 = HasArg(argc, argv, "--denoiser-v2");
    const int durationSec = GetArgInt(argc, argv, "--duration-sec", 0);
    const int statusIntervalMs =
        GetArgInt(argc, argv, "--status-interval-ms", 1000);

    std::string chosenSink =
        studiocast::util::TrimCopy(GetArgValue(argc, argv, "--sink"));
    if (chosenSink.empty()) {
      std::string err;
      const auto s = ChooseDefaultPhysicalSink(&err);
      if (!s) {
        std::cerr
            << "ERROR: Failed to choose a physical sink: " << err << "\n"
            << "Tip: pass an explicit --sink (see: studiocast-audio status).\n";
        return 2;
      }
      chosenSink = *s;
    }
    if (chosenSink == "studiocast_speakers" ||
        chosenSink == "studiocast_sink") {
      std::cerr << "ERROR: Refusing to play into '" << chosenSink
                << "' (feedback loop).\n";
      return 2;
    }

    {
      std::string err;
      if (!studiocast::audio::CreateVirtualSpeaker(&err)) {
        std::cerr << "ERROR: " << err << "\n";
        return 2;
      }
      // Avoid double-routing; best-effort.
      studiocast::audio::StopSpeakerLoopback(&err);
    }

    const auto settings = studiocast::config::LoadSettings();
    const auto sel = studiocast::maxine::SelectGpu(settings.gpu);
    if (!sel.selected || !sel.selected->compute_capability) {
      std::cerr << "ERROR: Failed to select a supported NVIDIA GPU. "
                << sel.error << "\n";
      return 2;
    }
    std::cout << "Selected GPU: index=" << sel.selected->index << ", name='"
              << sel.selected->name
              << "', compute_cap=" << sel.selected->ComputeCapString() << "\n";

    const auto paths = studiocast::maxine::ResolveMaxinePaths();
    if (!paths.afx.ok) {
      std::cerr << "ERROR: AFX SDK not available.\n";
      for (const auto &p : paths.afx.problems) {
        std::cerr << "  - " << p << "\n";
      }
      return 2;
    }

    studiocast::maxine::afx::AfxApi api;
    {
      std::string err;
      if (!api.InitializeFromLibraryPath(paths.afx.library, &err)) {
        std::cerr << "ERROR: Failed to initialize AFX runtime: " << err << "\n";
        return 2;
      }
    }

    auto plan = studiocast::maxine::afx::PlanBroadcastMicrophoneEffect(
        false, true, false, strength);
    plan.use_denoiser_v2_model = denoiserV2;
    if (!plan.enabled) {
      std::cerr << "ERROR: Speaker denoiser plan not enabled.\n";
      return 2;
    }

    studiocast::maxine::afx::AfxEffect fx(&api);
    {
      studiocast::maxine::afx::AfxEffectConfig cfg;
      cfg.effect_selector = plan.effect_selector;
      cfg.feature_id = plan.feature_id;
      cfg.features_dir = paths.afx.features_dir;
      cfg.compute_capability = sel.selected->compute_capability;
      cfg.sample_rate = 48000;
      cfg.frame_samples = 480;
      cfg.channels = 1;
      cfg.intensity = plan.intensity;
      cfg.use_denoiser_v2_model = plan.use_denoiser_v2_model;

      std::string err;
      if (!fx.Configure(cfg, &err)) {
        std::cerr << "ERROR: Failed to configure AFX effect: " << err << "\n";
        return 2;
      }
      if (!fx.Load(&err)) {
        std::cerr << "ERROR: Failed to load AFX effect: " << err << "\n";
        return 2;
      }
    }

    studiocast::maxine::afx::AfxAudioProcessor processor(&fx);
    studiocast::audio::AudioPipeline pipeline(&processor);
    {
      studiocast::audio::AudioPipelineConfig cfg;
      cfg.source_name = studiocast::audio::VirtualSpeakerMonitorSourceName();
      cfg.sink_name = chosenSink;

      std::string err;
      if (!pipeline.Start(cfg, &err)) {
        std::cerr << "ERROR: Failed to start speaker denoise pipeline: " << err
                  << "\n";
        return 2;
      }
    }

    g_stop.store(false, std::memory_order_release);
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    std::cout << "Speaker denoise pipeline running: source="
              << studiocast::audio::VirtualSpeakerMonitorSourceName()
              << " -> sink=" << chosenSink << ". Press Ctrl+C to stop.";
    if (durationSec > 0) {
      std::cout << " (auto-stop after " << durationSec << "s)";
    }
    std::cout << "\n";

    const auto start = std::chrono::steady_clock::now();
    while (!g_stop.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(statusIntervalMs));

      const auto s = pipeline.GetStats();
      if (!s.last_error.empty()) {
        std::cerr << "ERROR: " << s.last_error << "\n";
        break;
      }

      std::cout << "frames_processed=" << s.frames_processed << "\n";

      if (durationSec > 0) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start);
        if (elapsed.count() >= durationSec) {
          break;
        }
      }
    }

    pipeline.Stop();
    const auto finalStats = pipeline.GetStats();
    if (!finalStats.last_error.empty()) {
      std::cerr << "ERROR: " << finalStats.last_error << "\n";
      return 2;
    }

    std::cout << "Pipeline stopped. frames_processed="
              << finalStats.frames_processed << "\n";
    return 0;
#endif
  }

  Usage(argv[0]);
  return 1;
}
