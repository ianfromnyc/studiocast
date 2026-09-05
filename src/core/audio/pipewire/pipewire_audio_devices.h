#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/audio/audio_pipeline.h"
#include "core/audio/virtual_audio_service.h"
#include "core/pipewire/pipewire_audio_node.h"

namespace studiocast::audio::pw_backend {

// One PulseAudio module that the Pulse device path left behind.
struct StalePulseModule {
  int id = -1;
  std::string name; // e.g. "module-null-sink"
  std::string args;
};

// Finds the PulseAudio modules that belong to the StudioCast Pulse device
// path: the null sinks behind the virtual microphone and the virtual
// speakers, the remap source that carries the microphone, the legacy
// pass-through loopback into the StudioCast sink, and the speaker route.
//
// Pulse modules live in the sound server, so they outlive the process that
// loaded them. A native node dies with the process. Only the move from the
// Pulse path to the native path therefore needs a clean-up: without it the
// graph holds two nodes named `studiocast_mic` and an application can pick
// the stale one.
//
// The list is ordered for unloading: the loopbacks come first, then the remap
// source, then the null sinks it used.
//
// Two things are never in the list:
//  - the microphone monitor loopback, which reads `studiocast_mic` and works
//    the same on both backends. It carries the monitor stream name, and that
//    is what tells it apart.
//  - a module of another application, whatever it is named.
std::vector<StalePulseModule> DetectStalePulseDeviceModules(std::string *error);

// Unloads what DetectStalePulseDeviceModules finds. `removed` gets one
// human-readable line for each module that went away.
bool UnloadStalePulseDeviceModules(std::vector<std::string> *removed,
                                   std::string *error);

// What a process wants from the native device owner.
//
// The daemon keeps the defaults: canonical node names, and a clean-up of the
// PulseAudio device modules an earlier Pulse-backend run left behind. A test
// changes both, so that it never stands beside a running daemon: its nodes
// carry a suffix of their own, and it removes nothing from the sound server.
struct NativeAudioDeviceOptions {
  // Appended to the node name and to the node description of every device
  // this owner creates. Empty gives the canonical identities.
  std::string node_name_suffix;

  // Whether a create removes the stale Pulse device modules first.
  bool remove_stale_pulse_devices = true;
};

// Owns the native PipeWire virtual devices of the process.
//
// The Pulse path keeps its devices in the sound server, so they live longer
// than the pipeline. A native node lives only as long as the process holds the
// stream, so one owner keeps the nodes up while the daemon runs and the
// consumer-gated pipeline starts and stops.
class NativeAudioDevices final {
public:
  static NativeAudioDevices &Instance();

  // Takes effect on the next create. A device that already exists keeps the
  // name it was created with.
  void SetOptions(const NativeAudioDeviceOptions &options);
  NativeAudioDeviceOptions Options() const;

  bool CreateVirtualMic(std::string *error);
  bool DestroyVirtualMic(std::string *error);

  bool CreateVirtualSpeaker(std::string *error);
  bool DestroyVirtualSpeaker(std::string *error);

  // Pass-through route: virtual speakers -> a real sink, with no effects.
  bool StartSpeakerLoopback(const std::string &target_sink_name,
                            std::string *error);
  bool StopSpeakerLoopback(std::string *error);

  // What the last clean-up removed, one line for each module.
  std::vector<std::string> LastPulseCleanupLog() const;

  AudioConsumerSnapshot DetectMicrophoneConsumers() const;
  AudioConsumerSnapshot DetectSpeakerConsumers() const;

  // The node the microphone pipeline writes processed audio into, and the node
  // the speaker pipeline reads from. Null when the device is not created.
  studiocast::pw::PipeWireAudioNode *MicNode() const;
  studiocast::pw::PipeWireAudioNode *SpeakerNode() const;

private:
  NativeAudioDevices();
  ~NativeAudioDevices();

  // Removes the Pulse device modules an earlier run left in the sound server,
  // and writes one line for each to the daemon log.
  void RemoveStalePulseDevices();

  struct State;
  std::unique_ptr<State> state_;
};

// Takes down every native node this process owns.
//
// A native node dies with the process, so a restart needs nothing. This covers
// the move back to the Pulse path inside one running daemon, where the native
// nodes would otherwise stay in the graph beside the Pulse devices. It is safe
// to call when there is no node.
void ShutdownNativeAudioDevices();

// Real-time pipeline I/O over native PipeWire nodes.
//
// The microphone pipeline captures from a real source and writes into the
// virtual microphone node. The speaker pipeline, which sets
// `allow_monitor_source`, reads from the virtual speakers node and plays into
// the configured sink.
std::unique_ptr<AudioPipelineIo> CreatePipeWireAudioIo();

} // namespace studiocast::audio::pw_backend
