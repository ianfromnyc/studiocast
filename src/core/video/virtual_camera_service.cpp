#include "virtual_camera_service.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <sstream>
#include <thread>

#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstring>

#include "core/maxine/maxine_manager.h"
#include "../open_video/diagnose.h"
#include "core/util/proc.h"
#include "core/video/effects/broadcast_effect_maxine_gate.h"
#include "core/video/effects/broadcast_effect_open_cuda_gate.h"
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
           a.capture_mode != b.capture_mode ||
           a.width != b.width ||
           a.height != b.height ||
           a.fps != b.fps ||
           a.prefer_mjpeg != b.prefer_mjpeg ||
           a.scaling_backend != b.scaling_backend ||
           a.allow_cpu_resize != b.allow_cpu_resize;
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

    std::optional<studiocast::maxine::MaxineDiagnostics> maxineDiag;
    auto lastMaxineDiagAt = std::chrono::steady_clock::time_point{};

    std::optional<studiocast::open_cuda::OpenCudaDiagnostics> openCudaDiag;
    auto lastOpenCudaDiagAt = std::chrono::steady_clock::time_point{};

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
        // gated by consumerPresent below.
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

        auto effects_for_pipeline = cfg.pipeline.effects;
        bool effectsSuppressed = false;
        std::string suppressMsg;

        const bool wantRunRequested = cfg.enabled && consumerPresent;
        bool wantRun = wantRunRequested;

        // Gate expensive capture/processing threads based on engine availability.
        // Keep loopback output alive (see EnsureOutputOpen above) so consumers
        // still see a stable device.
        bool blocked = false;
        std::string blockedMsg;

        if (wantRunRequested) {
            const auto plan = effects::BuildBroadcastEffectsPlan(cfg.pipeline.effects);
            const std::set<std::string> planned(plan.ordered_effect_ids.begin(), plan.ordered_effect_ids.end());
            const auto has = [&](std::string_view id) {
                return planned.count(std::string(id)) != 0;
            };

            const bool wants_vb = has(effects::contract::kEffectIdVirtualBackgroundBlur) ||
                                  has(effects::contract::kEffectIdVirtualBackgroundRemove) ||
                                  has(effects::contract::kEffectIdVirtualBackgroundReplace);

            const bool wants_denoise = has(effects::contract::kEffectIdVideoNoiseRemoval);
            const bool wants_auto_frame = has(effects::contract::kEffectIdAutoFrame);
            const bool wants_key_light = has(effects::contract::kEffectIdVirtualKeyLight);
            const bool wants_open_cuda_fx = wants_vb || wants_denoise || wants_auto_frame || wants_key_light;

            // Effects that currently require Maxine (no Open CUDA implementation).
            const bool wants_maxine_only = has(effects::contract::kEffectIdEyeContact);

            const auto ttl = std::chrono::seconds(2);

            // --- Maxine gate ---
            bool needMaxineGate = false;
            bool needMaxineDiag = false;
            if (cfg.pipeline.effects.engine == effects::EffectsEnginePreference::maxine) {
                // Forced Maxine: any Maxine-backed effect blocks the pipeline when unavailable.
                needMaxineGate = effects::WantsMaxineForPlannedEffects(cfg.pipeline.effects);
                needMaxineDiag = needMaxineGate;
            } else if (cfg.pipeline.effects.engine == effects::EffectsEnginePreference::auto_select) {
                // Auto-select: only gate Maxine for effects that have no Open CUDA fallback.
                needMaxineGate = wants_maxine_only;
                // Still cache Maxine diagnostics if we need to decide whether Open CUDA effects can run on Maxine.
                needMaxineDiag = needMaxineGate || wants_open_cuda_fx;
            }

            if (needMaxineDiag) {
                if (!maxineDiag.has_value() || lastMaxineDiagAt == std::chrono::steady_clock::time_point{} ||
                    (now - lastMaxineDiagAt) >= ttl) {
                    studiocast::maxine::MaxineManager mgr;
                    maxineDiag = mgr.Diagnose(false);
                    lastMaxineDiagAt = now;
                }
            }

            if (needMaxineGate && maxineDiag.has_value()) {
                const auto gate = effects::EvaluateMaxineGate(cfg.pipeline.effects, *maxineDiag);
                if (!gate.ok) {
                    blocked = true;
                    blockedMsg = gate.message;
                    wantRun = false;
                }
            }

            // --- Open CUDA gate ---
            if (!blocked && wants_open_cuda_fx) {
                bool needOpenCudaGate = false;
                if (cfg.pipeline.effects.engine == effects::EffectsEnginePreference::open_cuda) {
                    needOpenCudaGate = true;
                } else if (cfg.pipeline.effects.engine == effects::EffectsEnginePreference::auto_select) {
                    // Auto-select: prefer Maxine when available; otherwise require Open CUDA for effects
                    // that have an Open CUDA implementation.
                    bool vb_available_in_maxine = false;
                    bool denoise_available_in_maxine = false;
                    bool auto_frame_available_in_maxine = false;
                    bool key_light_available_in_maxine = false;
                    if (maxineDiag.has_value()) {
                        const std::set<std::string> mx_avail(maxineDiag->available_effects.begin(),
                                                             maxineDiag->available_effects.end());
                        if (wants_denoise) {
                            denoise_available_in_maxine =
                                mx_avail.count(std::string(effects::contract::kEffectIdVideoNoiseRemoval)) != 0;
                        }
                        if (wants_vb) {
                            vb_available_in_maxine = true;
                            if (has(effects::contract::kEffectIdVirtualBackgroundBlur) &&
                                !mx_avail.count(std::string(effects::contract::kEffectIdVirtualBackgroundBlur))) {
                                vb_available_in_maxine = false;
                            }
                            if (has(effects::contract::kEffectIdVirtualBackgroundRemove) &&
                                !mx_avail.count(std::string(effects::contract::kEffectIdVirtualBackgroundRemove))) {
                                vb_available_in_maxine = false;
                            }
                            if (has(effects::contract::kEffectIdVirtualBackgroundReplace) &&
                                !mx_avail.count(std::string(effects::contract::kEffectIdVirtualBackgroundReplace))) {
                                vb_available_in_maxine = false;
                            }
                        }
                        if (wants_auto_frame) {
                            auto_frame_available_in_maxine =
                                mx_avail.count(std::string(effects::contract::kEffectIdAutoFrame)) != 0;
                        }
                        if (wants_key_light) {
                            key_light_available_in_maxine =
                                mx_avail.count(std::string(effects::contract::kEffectIdVirtualKeyLight)) != 0;
                        }
                    }

                    // If any requested Open CUDA-capable effect is not available in Maxine, require Open CUDA.
                    needOpenCudaGate = (wants_denoise && !denoise_available_in_maxine) ||
                                       (wants_vb && !vb_available_in_maxine) ||
                                       (wants_auto_frame && !auto_frame_available_in_maxine) ||
                                       (wants_key_light && !key_light_available_in_maxine);
                }

                if (needOpenCudaGate && effects::WantsOpenCudaForPlannedEffects(cfg.pipeline.effects)) {
                    if (!openCudaDiag.has_value() || lastOpenCudaDiagAt == std::chrono::steady_clock::time_point{} ||
                        (now - lastOpenCudaDiagAt) >= ttl) {
                        openCudaDiag = studiocast::open_cuda::DiagnoseOpenCudaDefault();
                        lastOpenCudaDiagAt = now;
                    }

                    if (openCudaDiag.has_value()) {
                        const auto gate = effects::EvaluateOpenCudaGate(cfg.pipeline.effects, *openCudaDiag);
                        if (!gate.ok) {
                            effectsSuppressed = true;
                            suppressMsg = gate.message;
                            effects_for_pipeline.virtual_background.mode = effects::VirtualBackgroundMode::none;
                            effects_for_pipeline.video_noise_removal.enabled = false;
                            effects_for_pipeline.auto_frame.enabled = false;
                            effects_for_pipeline.virtual_key_light.enabled = false;
                        }
                    }
                }
            }
        }

        // Apply effects live regardless of consumer state.
        pipeline_.SetEffects(effects_for_pipeline);

        auto pipelineCfgForPipeline = cfg.pipeline;
        pipelineCfgForPipeline.effects = effects_for_pipeline;

        auto pst = pipeline_.Status();
        if (blocked) {
            {
                std::lock_guard<std::mutex> lock(mu_);
                if (last_error_ != blockedMsg) last_error_ = blockedMsg;
            }

            if (pst.running || pst.starting) {
                pipeline_.Stop();
                haveAppliedCfg = false;
                nextStartRetry = std::chrono::steady_clock::time_point{};
                pst = pipeline_.Status();
            }
        } else if (effectsSuppressed) {
            std::lock_guard<std::mutex> lock(mu_);
            if (last_error_ != suppressMsg) last_error_ = suppressMsg;
        }

        // Restart if config changed and we are running.
        if (pst.running && haveAppliedCfg && NeedsPipelineRestart(appliedPipelineCfg, pipelineCfgForPipeline)) {
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
                    if (!pipeline_.Start(pipelineCfgForPipeline, &perr)) {
                        {
                            std::lock_guard<std::mutex> lock(mu_);
                            last_error_ = "Pipeline start failed: " + perr;
                        }
                        // Back off a bit; many apps probe cameras aggressively.
                        nextStartRetry = now + std::chrono::seconds(2);
                    } else {
                        haveAppliedCfg = true;
                        appliedPipelineCfg = pipelineCfgForPipeline;
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
