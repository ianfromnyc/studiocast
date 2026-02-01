#pragma once

#include <string>

namespace studiocast::audio {

// Creates a virtual microphone named "studiocast_mic" backed by a null sink +
// remap-source. Safe to call repeatedly (idempotent-ish).
bool CreateVirtualMic(std::string *error);

// Stops loopback (if any) and destroys the virtual mic modules.
bool DestroyVirtualMic(std::string *error);

// Starts loopback from a real source -> StudioCast sink.
// If source_name is empty, uses default source.
bool StartLoopback(const std::string &source_name, int latency_ms,
                   std::string *error);

// Stops loopback (only the ones pointing to the StudioCast sink).
bool StopLoopback(std::string *error);

// Prints a human-readable status string (for CLI convenience).
std::string StatusText();

} // namespace studiocast::audio
