#include "virtual_camera_service.h"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <thread>

#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstring>

#include "core/util/proc.h"
#include "core/video/v4l2loopback.h"

namespace studiocast::video {
namespace {

std::string ChooseDefaultOutputLoopback(std::string* error) {
    const auto rep = ProbeLoopback();
    for (const auto& d : rep.devices) {
        if (d.is_loopback && d.can_write) return d.dev_node;
    }
    if (error) {
        std::ostringstream oss;
        oss << "No writable v4l2loopback device found.\n"
            << "Run the suggested command from studiocast-video status, e.g.:\n"
            << "  " << rep.suggested_modprobe_cmd << "\n";
        *error = oss.str();
    }
    return {};
}

}  // namespace

VirtualCameraService::~VirtualCameraService() {
    Stop();
}

bool VirtualCameraService::NeedsPipelineRestart(const CameraPipelineConfig& a, const CameraPipelineConfig& b) {
    // Mirror (and other future live-toggles) can be applied without restart.
    return a.input_device != b.input_device ||
           a.output_device != b.output_device ||
           a.width != b.width ||
           a.height != b.height ||
           a.fps != b.fps;
}

bool VirtualCameraService::Start(const VirtualCameraServiceConfig& cfg, std::string* error) {
    std::thread toJoin;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (running_) {
            if (error) *error = "VirtualCameraService already running.";
            return false;
        }

        if (th_.joinable()) {
            // A previous thread finished but wasn't joined (should be rare). Join it now.
            toJoin = std::move(th_);
        }

        cfg_ = cfg;
        last_error_.clear();
        consumer_present_ = false;
        consumer_count_ = 0;
        last_consumer_seen_ = std::chrono::steady_clock::time_point{};
        next_start_retry_ = std::chrono::steady_clock::time_point{};

        stop_.store(false);
        running_ = true;
    }

    if (toJoin.joinable()) {
        toJoin.join();
    }

    // Best-effort: open the v4l2loopback output immediately so apps like OBS can discover it.
    // This also helps avoid a small race where the supervisor thread hasn't yet run its first
    // poll cycle when the user opens an app.
    {
        VirtualCameraServiceConfig localCfg;
        {
            std::lock_guard<std::mutex> lock(mu_);
            localCfg = cfg_;
        }

        if (localCfg.pipeline.output_device.empty()) {
            std::string e;
            const auto out = ChooseDefaultOutputLoopback(&e);
            if (!out.empty()) {
                localCfg.pipeline.output_device = out;
                std::lock_guard<std::mutex> lock(mu_);
                cfg_.pipeline.output_device = out;
            } else if (!e.empty()) {
                std::lock_guard<std::mutex> lock(mu_);
                last_error_ = e;
            }
        }

        if (!localCfg.pipeline.output_device.empty()) {
            std::string oerr;
            if (!pipeline_.EnsureOutputOpen(localCfg.pipeline, &oerr)) {
                std::lock_guard<std::mutex> lock(mu_);
                last_error_ = "Output open failed: " + oerr;
            }
        }
    }

    // Launch supervisor thread outside the lock to avoid deadlocks.
    std::thread t(&VirtualCameraService::ThreadMain, this);
    {
        std::lock_guard<std::mutex> lock(mu_);
        th_ = std::move(t);
    }

    return true;
}

void VirtualCameraService::Stop() {
    stop_.store(true);

    std::thread toJoin;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (th_.joinable()) toJoin = std::move(th_);
        running_ = false;
    }

    if (toJoin.joinable()) toJoin.join();

    // Ensure the pipeline is stopped even if the supervisor thread exits early.
    pipeline_.Stop();

    std::lock_guard<std::mutex> lock(mu_);
    consumer_present_ = false;
    consumer_count_ = 0;
}

void VirtualCameraService::UpdateConfig(const VirtualCameraServiceConfig& cfg) {
    std::lock_guard<std::mutex> lock(mu_);
    cfg_ = cfg;
}

VirtualCameraServiceStatus VirtualCameraService::Status() const {
    std::lock_guard<std::mutex> lock(mu_);
    VirtualCameraServiceStatus s;
    s.service_running = running_;
    s.consumer_present = consumer_present_;
    s.consumer_count = consumer_count_;
    s.pipeline = pipeline_.Status();
    s.last_error = last_error_;
    return s;
}

VirtualCameraServiceConfig VirtualCameraService::Config() const {
    std::lock_guard<std::mutex> lock(mu_);
    return cfg_;
}

void VirtualCameraService::ThreadMain() {
    const int selfPid = static_cast<int>(::getpid());

    // This runs independently of the camera pipeline thread.
    auto lastConsumerSeen = std::chrono::steady_clock::now();
    auto nextStartRetry = std::chrono::steady_clock::time_point{};

    bool haveAppliedCfg = false;
    CameraPipelineConfig appliedPipelineCfg{};

    while (!stop_.load()) {
        VirtualCameraServiceConfig cfg;
        {
            std::lock_guard<std::mutex> lock(mu_);
            cfg = cfg_;
        }

        // If the configured output device disappeared (e.g., v4l2loopback was reloaded),
        // clear it so we can re-probe.
        if (!cfg.pipeline.output_device.empty()) {
            struct stat st {};
            if (::stat(cfg.pipeline.output_device.c_str(), &st) != 0) {
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    last_error_ = "Output device " + cfg.pipeline.output_device + " not available: " +
                                 std::string(std::strerror(errno));
                    cfg_.pipeline.output_device.clear();
                    consumer_present_ = false;
                    consumer_count_ = 0;
                }

                pipeline_.Stop();
                haveAppliedCfg = false;

                std::this_thread::sleep_for(std::chrono::milliseconds(std::max(50, cfg.consumer_poll_ms)));
                continue;
            }
        }

        // Ensure we have an output device to monitor (and to start the pipeline).
        if (cfg.pipeline.output_device.empty()) {
            std::string e;
            const auto out = ChooseDefaultOutputLoopback(&e);
            if (!out.empty()) {
                cfg.pipeline.output_device = out;
                // Persist best-effort into shared cfg_ so we don't re-probe every loop.
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    cfg_.pipeline.output_device = out;
                }
            } else {
                // No loopback yet: record error and wait.
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    last_error_ = e;
                    consumer_present_ = false;
                    consumer_count_ = 0;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(std::max(50, cfg.consumer_poll_ms)));
                continue;
            }
        }

        // IMPORTANT: When v4l2loopback is loaded with exclusive_caps=1, the
        // device announces OUTPUT-only until a producer opens it. Many apps
        // will not list / open the virtual camera until it reports CAPTURE.
        //
        // Keep the output open even when we're idle so consumers can discover
        // the device. Heavy processing (camera capture + effects) is still
        // gated by consumerPresent/always_on below.
        {
            std::string oerr;
            if (!pipeline_.EnsureOutputOpen(cfg.pipeline, &oerr)) {
                std::lock_guard<std::mutex> lock(mu_);
                last_error_ = "Output open failed: " + oerr;
            }
        }

        // Detect consumers of the loopback device.
        bool consumerPresent = false;
        int consumerCount = 0;
        {
            std::string scanErr;
            util::OpenFileScanOptions opt;
            opt.exclude_pid = selfPid;
            opt.stop_at_first = false;

            const auto pids = util::PidsWithOpenFile(cfg.pipeline.output_device, opt, &scanErr);
            consumerCount = static_cast<int>(pids.size());
            consumerPresent = consumerCount > 0;

            // Non-fatal: keep running even if scan can't see all processes.
            if (!scanErr.empty()) {
                std::lock_guard<std::mutex> lock(mu_);
                last_error_ = scanErr;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (consumerPresent) lastConsumerSeen = now;

        // Apply effects live regardless of consumer state.
        pipeline_.SetEffects(cfg.pipeline.effects);

        const bool wantRun = cfg.enabled && (cfg.always_on || consumerPresent);
        const auto pst = pipeline_.Status();

        // Restart if config changed and we are running.
        if (pst.running && haveAppliedCfg && NeedsPipelineRestart(appliedPipelineCfg, cfg.pipeline)) {
            pipeline_.Stop();
            haveAppliedCfg = false;
            nextStartRetry = std::chrono::steady_clock::time_point{};
        }

        if (wantRun) {
            if (!pst.running && !pst.starting) {
                if (nextStartRetry != std::chrono::steady_clock::time_point{} && now < nextStartRetry) {
                    // still in backoff window
                } else {
                    std::string perr;
                    if (!pipeline_.Start(cfg.pipeline, &perr)) {
                        {
                            std::lock_guard<std::mutex> lock(mu_);
                            last_error_ = "Pipeline start failed: " + perr;
                        }
                        // Back off a bit; many apps probe cameras aggressively.
                        nextStartRetry = now + std::chrono::seconds(2);
                    } else {
                        haveAppliedCfg = true;
                        appliedPipelineCfg = cfg.pipeline;
                        nextStartRetry = std::chrono::steady_clock::time_point{};
                    }
                }
            }
        } else {
            if (pst.running || pst.starting) {
                const int graceMs = std::max(0, cfg.stop_grace_ms);
                const auto grace = std::chrono::milliseconds(graceMs);

                if (graceMs == 0 || (now - lastConsumerSeen) >= grace) {
                    pipeline_.Stop();
                    haveAppliedCfg = false;
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(mu_);
            consumer_present_ = consumerPresent;
            consumer_count_ = consumerCount;
            last_consumer_seen_ = lastConsumerSeen;
            next_start_retry_ = nextStartRetry;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(std::max(50, cfg.consumer_poll_ms)));
    }

    // Ensure pipeline stops when the supervisor exits.
    pipeline_.Stop();

    std::lock_guard<std::mutex> lock(mu_);
    running_ = false;
}

}  // namespace studiocast::video
