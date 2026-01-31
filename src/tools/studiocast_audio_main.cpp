#include <iostream>
#include <string>
#include <string_view>

#include "core/audio/virtual_mic.h"

namespace {

void Usage(const char* argv0) {
  std::cout
      << "StudioCast Audio Tool\n\n"
      << "Usage:\n"
      << "  " << argv0 << " status\n"
      << "  " << argv0 << " create\n"
      << "  " << argv0 << " destroy\n"
      << "  " << argv0 << " loopback-start [--source <name>] [--latency-ms <n>]\n"
      << "  " << argv0 << " loopback-stop\n";
}

std::string GetArgValue(int argc, char** argv, std::string_view key) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (argv[i] && std::string_view(argv[i]) == key) {
      return argv[i + 1] ? std::string(argv[i + 1]) : "";
    }
  }
  return "";
}

int GetArgInt(int argc, char** argv, std::string_view key, int fallback) {
  const auto v = GetArgValue(argc, argv, key);
  if (v.empty()) return fallback;
  return std::atoi(v.c_str());
}

}  // namespace

int main(int argc, char** argv) {
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

  if (cmd == "loopback-start") {
    const std::string source = GetArgValue(argc, argv, "--source");
    const int latency = GetArgInt(argc, argv, "--latency-ms", 10);

    std::string err;
    if (!studiocast::audio::StartLoopback(source, latency, &err)) {
      std::cerr << "ERROR: " << err << "\n";
      return 2;
    }

    std::cout << "Loopback started (source=" << (source.empty() ? "<default>" : source)
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

  Usage(argv[0]);
  return 1;
}
