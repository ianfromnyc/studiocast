#pragma once

#include <optional>
#include <string>
#include <vector>

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

    struct PactlPort {
        std::string name;         // e.g. "analog-input-internal-mic"
        std::string description;  // e.g. "Internal Microphone"
        bool available = true;
    };

    struct PactlSourceInfo {
        int id = -1;
        std::string name;         // Pulse source name
        std::string description;  // Human-friendly description (may be empty)
        std::string active_port;  // Port name (may be empty)
        std::vector<PactlPort> ports;
    };

    std::vector<PactlSourceInfo> ListSourcesDetailed(std::string* error);
    bool SetSourcePort(const std::string& source_name, const std::string& port_name, std::string* error);

    bool PactlAvailable(std::string* details);

    std::optional<int> LoadModule(const std::string& module, const std::string& args, std::string* error);
    bool UnloadModule(int id, std::string* error);

    std::vector<PactlModule> ListModules(std::string* error);
    std::vector<PactlSource> ListSources(std::string* error);

    std::optional<std::string> GetDefaultSourceName(std::string* error);

    bool UpdateSinkProplist(const std::string& sink_name_or_index,
                        const std::vector<std::string>& kv_pairs,
                        std::string* error);

    bool UpdateSourceProplist(const std::string& source_name_or_index,
                              const std::vector<std::string>& kv_pairs,
                              std::string* error);

}  // namespace studiocast::audio::pulse
