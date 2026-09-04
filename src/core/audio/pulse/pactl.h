#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/util/exec.h"

namespace studiocast::audio::pulse {

struct PactlModule {
  int id = -1;
  std::string name;
  std::string args;
};

struct PactlSource {
  int id = -1;
  std::string name;
};

struct PactlSink {
  int id = -1;
  std::string name;
};

struct PactlSourceOutput {
  int id = -1;
  std::string source;
};

struct PactlSinkInput {
  int id = -1;
  std::string sink;
};

struct PactlSinkInputInfo {
  int id = -1;
  int owner_module = -1; // -1 when the stream has no owner module
  std::string sink;      // sink index or name as reported by pactl
  std::string media_name;
};

struct PactlPort {
  std::string name;        // e.g. "analog-input-internal-mic"
  std::string description; // e.g. "Internal Microphone"
  bool available = true;
};

struct PactlSourceInfo {
  int id = -1;
  std::string name;        // Pulse source name
  std::string description; // Human-friendly description (may be empty)
  std::string active_port; // Port name (may be empty)
  std::vector<PactlPort> ports;
};

std::vector<PactlSourceInfo> ListSourcesDetailed(std::string *error);
bool SetSourcePort(const std::string &source_name, const std::string &port_name,
                   std::string *error);

bool PactlAvailable(std::string *details);

using PactlExecCaptureHook =
    std::function<studiocast::util::ExecResult(const std::string &)>;

void SetPactlExecCaptureHookForTesting(PactlExecCaptureHook hook);

std::optional<int> LoadModule(const std::string &module,
                              const std::string &args, std::string *error);
std::optional<int> LoadModule(const std::string &module,
                              const std::vector<std::string> &args,
                              std::string *error);
bool UnloadModule(int id, std::string *error);

std::vector<PactlModule> ListModules(std::string *error);
std::vector<PactlSource> ListSources(std::string *error);
std::vector<PactlSink> ListSinks(std::string *error);
std::vector<PactlSourceOutput> ListSourceOutputs(std::string *error);
std::vector<PactlSinkInput> ListSinkInputs(std::string *error);

std::optional<std::string> GetDefaultSourceName(std::string *error);
std::optional<std::string> GetDefaultSinkName(std::string *error);

// Deterministic parsing helper (used for `pactl info` fallbacks and self-test).
// Example keys: "Default Source:", "Default Sink:".
std::optional<std::string>
ParseDefaultFromPactlInfo(const std::string &pactl_info_text,
                          const std::string &key);

bool UpdateSinkProplist(const std::string &sink_name_or_index,
                        const std::vector<std::string> &kv_pairs,
                        std::string *error);

bool UpdateSourceProplist(const std::string &source_name_or_index,
                          const std::vector<std::string> &kv_pairs,
                          std::string *error);

// Detailed sink input list. Use it to find a stream by its owner module or by
// a property that StudioCast set when the stream was created.
std::vector<PactlSinkInputInfo> ListSinkInputsDetailed(std::string *error);

// Sets the volume of one sink input, in percent (0..100 and above).
bool SetSinkInputVolumePercent(int sink_input_id, int percent,
                               std::string *error);

} // namespace studiocast::audio::pulse
