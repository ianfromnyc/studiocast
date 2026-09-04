#pragma once

#include <memory>
#include <string>

#include "core/audio/audio_pipeline.h"
#include "core/audio/virtual_audio_service.h"
#include "core/pipewire/pipewire_audio_node.h"

namespace studiocast::audio::pw_backend {

// Owns the native PipeWire virtual devices of the process.
//
// The Pulse path keeps its devices in the sound server, so they live longer
// than the pipeline. A native node lives only as long as the process holds the
// stream, so one owner keeps the nodes up while the daemon runs and the
// consumer-gated pipeline starts and stops.
class NativeAudioDevices final {
public:
  static NativeAudioDevices &Instance();

  bool CreateVirtualMic(std::string *error);
  bool DestroyVirtualMic(std::string *error);

  bool CreateVirtualSpeaker(std::string *error);
  bool DestroyVirtualSpeaker(std::string *error);

  // Pass-through route: virtual speakers -> a real sink, with no effects.
  bool StartSpeakerLoopback(const std::string &target_sink_name,
                            std::string *error);
  bool StopSpeakerLoopback(std::string *error);

  AudioConsumerSnapshot DetectMicrophoneConsumers() const;
  AudioConsumerSnapshot DetectSpeakerConsumers() const;

  // The node the microphone pipeline writes processed audio into, and the node
  // the speaker pipeline reads from. Null when the device is not created.
  studiocast::pw::PipeWireAudioNode *MicNode() const;
  studiocast::pw::PipeWireAudioNode *SpeakerNode() const;

private:
  NativeAudioDevices();
  ~NativeAudioDevices();

  struct State;
  std::unique_ptr<State> state_;
};

// Real-time pipeline I/O over native PipeWire nodes.
//
// The microphone pipeline captures from a real source and writes into the
// virtual microphone node. The speaker pipeline, which sets
// `allow_monitor_source`, reads from the virtual speakers node and plays into
// the configured sink.
std::unique_ptr<AudioPipelineIo> CreatePipeWireAudioIo();

} // namespace studiocast::audio::pw_backend
