#pragma once

#include <string>

#include "core/audio/virtual_speaker_state.h"

namespace studiocast::audio {

// Creates a virtual speaker sink named "studiocast_speakers".
// Apps select this sink as their output, and StudioCast routes its monitor stream
// into a real (physical) sink via loopback or an optional processed pipeline.
bool CreateVirtualSpeaker(std::string* error);

// Stops loopback (if any) and destroys the virtual speaker sink.
bool DestroyVirtualSpeaker(std::string* error);

// Starts pass-through playback: `studiocast_speakers.monitor` -> target sink.
// If `target_sink_name` is empty, uses Pulse default sink.
bool StartSpeakerLoopback(const std::string& target_sink_name, int latency_ms, std::string* error);

// Stops any loopbacks sourced from `studiocast_speakers.monitor`.
bool StopSpeakerLoopback(std::string* error);

// Returns the Pulse monitor source name for the virtual speaker sink.
std::string VirtualSpeakerMonitorSourceName();

// Debug/status helper: detects currently loaded modules by scanning `pactl list short modules`.
VirtualSpeakerState DetectVirtualSpeakerLoaded();

}  // namespace studiocast::audio
