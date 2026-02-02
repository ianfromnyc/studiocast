#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>

#include "core/video/virtual_camera_service.h"
#include "core/video/v4l2loopback.h"
#include "studiocast/version.h"

namespace {

std::atomic_bool g_running{true};

void HandleSignal(int) {
    g_running.store(false);
}

bool HasArg(int argc, char** argv, const std::string& flag) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i] && flag == argv[i]) return true;
    }
    return false;
}

std::string GetArgValue(int argc, char** argv, const std::string& key) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] && key == argv[i]) {
            return argv[i + 1] ? std::string(argv[i + 1]) : std::string();
        }
    }
    return {};
}

int GetArgInt(int argc, char** argv, const std::string& key, int fallback) {
    const auto v = GetArgValue(argc, argv, key);
    if (v.empty()) return fallback;
    return std::atoi(v.c_str());
}

void Usage(const char* argv0) {
    std::cout
        << "StudioCast background service (studiocastd)\n\n"
        << "Usage:\n"
        << "  " << argv0 << " [options]\n\n"
        << "Options:\n"
        << "  --input /dev/videoX      Input camera (default: auto)\n"
        << "  --output /dev/videoY     Output v4l2loopback (default: auto)\n"
        << "  --width N                Requested width (default: 1280)\n"
        << "  --height N               Requested height (default: 720)\n"
        << "  --fps N                  Requested fps (default: 30)\n"
        << "  --mirror                 Enable mirror (horizontal flip)\n"
        << "  --poll-ms N              Consumer poll interval (default: 250)\n"
        << "  --stop-grace-ms N        Stop after N ms without consumers (default: 1000)\n"
        << "  --always-on              Run pipeline even with no consumers\n"
        << "  --version                Print version and exit\n"
        << "  -h, --help               Show this help\n\n"
        << "Notes:\n"
        << "  - This daemon does NOT run modprobe for you.\n"
        << "  - Consumer-driven start/stop is based on scanning /proc/*/fd for open handles\n"
        << "    to the v4l2loopback device (best-effort; typically works when OBS/Zoom run\n"
        << "    under the same user).\n";
}

std::string ChooseWritableLoopbackDevice() {
    const auto rep = studiocast::video::ProbeLoopback();
    for (const auto& d : rep.devices) {
        if (d.is_loopback && d.can_write) return d.dev_node;
    }
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    if (HasArg(argc, argv, "--version") || HasArg(argc, argv, "-v")) {
        std::printf("studiocastd %s (%s)\n", STUDIOCAST_VERSION, STUDIOCAST_GIT_SHA);
        return 0;
    }

    if (HasArg(argc, argv, "--help") || HasArg(argc, argv, "-h")) {
        Usage(argv[0]);
        return 0;
    }

    studiocast::video::VirtualCameraServiceConfig cfg;

    cfg.pipeline.input_device = GetArgValue(argc, argv, "--input");
    cfg.pipeline.output_device = GetArgValue(argc, argv, "--output");

    cfg.pipeline.width = GetArgInt(argc, argv, "--width", 1280);
    cfg.pipeline.height = GetArgInt(argc, argv, "--height", 720);
    cfg.pipeline.fps = GetArgInt(argc, argv, "--fps", 30);

    cfg.pipeline.effects.mirror = HasArg(argc, argv, "--mirror");

    cfg.consumer_poll_ms = GetArgInt(argc, argv, "--poll-ms", 250);
    cfg.stop_grace_ms = GetArgInt(argc, argv, "--stop-grace-ms", 1000);
    cfg.always_on = HasArg(argc, argv, "--always-on");

    if (cfg.pipeline.output_device.empty()) {
        // If possible, pre-fill output for a nicer startup experience.
        cfg.pipeline.output_device = ChooseWritableLoopbackDevice();
    }

    const auto rep = studiocast::video::ProbeLoopback();
    if (!rep.ReadyForVirtualCamera()) {
        std::cout << rep.ToText() << "\n\n";
    }

    std::cout << "studiocastd " << STUDIOCAST_VERSION << " (" << STUDIOCAST_GIT_SHA << ")\n";
    std::cout << "Virtual camera supervisor started. Press Ctrl+C to stop.\n\n";

    studiocast::video::VirtualCameraService svc;
    std::string err;
    if (!svc.Start(cfg, &err)) {
        std::cerr << "ERROR: " << err << "\n";
        return 1;
    }

    studiocast::video::VirtualCameraServiceStatus prev;

    // Print initial status once so users immediately see which devices were selected
    // (especially when running in auto mode).
    {
        const auto st = svc.Status();
        std::cout << "[status] consumers=" << st.consumer_count
                  << " running=" << (st.pipeline.running ? "yes" : (st.pipeline.starting ? "starting" : "no"))
                  << " in=" << (st.pipeline.input_device.empty() ? "(auto)" : st.pipeline.input_device)
                  << " out=" << (st.pipeline.output_device.empty() ? "(auto)" : st.pipeline.output_device)
                  << "\n";
        if (!st.last_error.empty()) {
            std::cout << "[last_error] " << st.last_error << "\n";
        }
        std::cout.flush();
        prev = st;
    }

    while (g_running.load()) {
        const auto st = svc.Status();

        // Print state transitions.
        if (st.consumer_present != prev.consumer_present ||
            st.pipeline.running != prev.pipeline.running ||
            st.pipeline.starting != prev.pipeline.starting ||
            st.pipeline.input_device != prev.pipeline.input_device ||
            st.pipeline.output_device != prev.pipeline.output_device ||
            st.last_error != prev.last_error) {

            std::cout << "[status] consumers=" << st.consumer_count
                      << " running=" << (st.pipeline.running ? "yes" : (st.pipeline.starting ? "starting" : "no"))
                      << " in=" << (st.pipeline.input_device.empty() ? "(auto)" : st.pipeline.input_device)
                      << " out=" << (st.pipeline.output_device.empty() ? "(auto)" : st.pipeline.output_device)
                      << "\n";

            if (!st.last_error.empty()) {
                std::cout << "[last_error] " << st.last_error << "\n";
            }

            std::cout.flush();
            prev = st;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    std::cout << "\nStopping...\n";
    svc.Stop();
    return 0;
}
