#pragma once

#include <string>

namespace studiocast::audio {

// Creates a virtual microphone named "studiocast_mic" backed by a null sink +
// remap-source.
//
// Processed mode (production):
//   - Create only: module-null-sink ("studiocast_sink") + module-remap-source
//   ("studiocast_mic")
//   - The real-time pipeline is expected to PLAY processed audio into
//   "studiocast_sink".
//   - Apps consume "studiocast_mic" which is the sink monitor.
// Safe to call repeatedly (idempotent-ish).
bool CreateVirtualMic(std::string *error);

// Stops loopback (if any) and destroys the virtual mic modules.
bool DestroyVirtualMic(std::string *error);

// Legacy pass-through mode (debug-only): starts loopback from a real source ->
// StudioCast sink.
//
// This is NOT used for the processed feed (Maxine AFX) and is intentionally
// disabled in production/release builds to avoid accidental double-routing or
// feedback loops. If source_name is empty, uses default source.
bool StartLoopback(const std::string &source_name, int latency_ms,
                   std::string *error);

// Stops loopback (only the ones pointing to the StudioCast sink).
bool StopLoopback(std::string *error);

// Prints a human-readable status string (for CLI convenience).
std::string StatusText();

} // namespace studiocast::audio
