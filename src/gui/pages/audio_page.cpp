#include "audio_page.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <optional>
#include <string>
#include <vector>

#include "core/audio/pulse/pactl.h"
#include "core/audio/virtual_mic.h"
#include "core/audio/virtual_speaker.h"
#include "core/ipc/daemon_client.h"
#include "core/maxine/reason_codes.h"

namespace studiocast::gui {
    namespace {
        constexpr const char *kStudioCastVirtualMicName = "studiocast_mic";

        bool IsBadLoopbackSourceCandidate(const std::string &name) {
            // Avoid obvious feedback loops / non-mic candidates:
            // - our own virtual mic
            // - monitor sources (sink monitors)
            if (name == kStudioCastVirtualMicName) return true;
            if (name.find(".monitor") != std::string::npos) return true;
            return false;
        }

        bool Contains(const std::string &hay, const std::string &needle) {
            return hay.find(needle) != std::string::npos;
        }

        bool ParseJsonObject(const std::string& json, QJsonObject* outRoot, QString* error) {
            QJsonParseError perr;
            const auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(json), &perr);
            if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
                if (error) *error = "JSON parse error: " + perr.errorString();
                return false;
            }
            if (outRoot) *outRoot = doc.object();
            return true;
        }

        QString FormatMaxineReasonCode(const QString& code) {
            if (code.isEmpty()) return {};
            const std::string s = code.toStdString();
            return QString::fromStdString(studiocast::maxine::reasons::ToEnglish(s));
        }

        bool DaemonRequest(const std::string& request, std::string* outJson, QString* outErr) {
            studiocast::ipc::DaemonCallResult res;
            std::string err;
            if (!studiocast::ipc::DaemonCall(request, &res, &err)) {
                if (outErr) *outErr = QString::fromStdString(err);
                return false;
            }
            if (!res.ok) {
                if (outErr) *outErr = QString::fromStdString(res.error_json.empty() ? "daemon_error" : res.error_json);
                return false;
            }
            if (outJson) *outJson = res.json;
            return true;
        }
    } // namespace

    AudioPage::AudioPage(QWidget *parent) : QWidget(parent) {
        auto *root = new QVBoxLayout(this);
        root->setSpacing(12);

        auto *title = new QLabel("Microphone", this);
        title->setStyleSheet("font-size: 20px; font-weight: 600;");
        root->addWidget(title);

        // -----------------------
        // AI effects (daemon-driven)
        // -----------------------
        auto* aiBox = new QGroupBox("AI microphone effects (Maxine AFX via daemon)", this);
        auto* aiLayout = new QVBoxLayout(aiBox);

        aiBanner_ = new QLabel(aiBox);
        aiBanner_->setWordWrap(true);
        aiBanner_->setStyleSheet(
            "background: #3a1414; border: 1px solid #663333; color: #f0d0d0; padding: 8px; border-radius: 4px;");
        aiBanner_->setVisible(false);
        aiLayout->addWidget(aiBanner_);

        auto* aiRow = new QHBoxLayout();
        noiseRemovalCb_ = new QCheckBox("Noise removal", aiBox);
        echoRemovalCb_ = new QCheckBox("Echo removal", aiBox);
        studioVoiceCb_ = new QCheckBox("Studio Voice", aiBox);
        aiRow->addWidget(noiseRemovalCb_);
        aiRow->addWidget(echoRemovalCb_);
        aiRow->addWidget(studioVoiceCb_);
        aiRow->addStretch(1);
        aiLayout->addLayout(aiRow);

        auto* strengthRow = new QHBoxLayout();
        strengthRow->addWidget(new QLabel("Strength:", aiBox));
        strengthSlider_ = new QSlider(Qt::Horizontal, aiBox);
        strengthSlider_->setRange(0, 100);
        strengthSlider_->setValue(50);
        strengthRow->addWidget(strengthSlider_, 1);
        strengthValueLabel_ = new QLabel("50", aiBox);
        strengthValueLabel_->setMinimumWidth(32);
        strengthValueLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        strengthRow->addWidget(strengthValueLabel_);
        aiLayout->addLayout(strengthRow);

        auto* aiBtnRow = new QHBoxLayout();
        aiStartBtn_ = new QPushButton("Start", aiBox);
        aiStopBtn_ = new QPushButton("Stop", aiBox);
        aiRefreshBtn_ = new QPushButton("Refresh", aiBox);
        aiBtnRow->addWidget(aiStartBtn_);
        aiBtnRow->addWidget(aiStopBtn_);
        aiBtnRow->addSpacing(12);
        aiBtnRow->addWidget(aiRefreshBtn_);
        aiBtnRow->addStretch(1);
        aiLayout->addLayout(aiBtnRow);

        aiLayout->addWidget(new QLabel(
            "Noise removal and echo removal share a single strength slider (Broadcast behavior).\n"
            "Studio Voice is exclusive with noise/echo removal.",
            aiBox));

        root->addWidget(aiBox);

        // -----------------------
        // Input selection
        // -----------------------
        auto *inputBox = new QGroupBox("Input (legacy loopback)", this);
        auto *inputLayout = new QVBoxLayout(inputBox);

        auto *sourceRow = new QHBoxLayout();
        sourceRow->addWidget(new QLabel("Input source:", inputBox));

        sourceCombo_ = new QComboBox(inputBox);
        sourceRow->addWidget(sourceCombo_, 1);

        refreshSourcesBtn_ = new QPushButton("Refresh", inputBox);
        sourceRow->addWidget(refreshSourcesBtn_);

        inputLayout->addLayout(sourceRow);

        // NEW: Port selection
        auto *portRow = new QHBoxLayout();
        portRow->addWidget(new QLabel("Input port:", inputBox));

        portCombo_ = new QComboBox(inputBox);
        portRow->addWidget(portCombo_, 1);

        inputLayout->addLayout(portRow);

        auto *latencyRow = new QHBoxLayout();
        latencyRow->addWidget(new QLabel("Latency (ms):", inputBox));

        latencySpin_ = new QSpinBox(inputBox);
        latencySpin_->setRange(1, 200);
        latencySpin_->setValue(10);
        latencyRow->addWidget(latencySpin_);

        latencyRow->addStretch(1);
        inputLayout->addLayout(latencyRow);

        root->addWidget(inputBox);

        // -----------------------
        // Virtual mic controls
        // -----------------------
        auto *vmicBox = new QGroupBox("StudioCast Virtual Microphone", this);
        auto *vmicLayout = new QVBoxLayout(vmicBox);

        auto *buttonsRow = new QHBoxLayout();
        createBtn_ = new QPushButton("Create virtual mic", vmicBox);
        destroyBtn_ = new QPushButton("Destroy virtual mic", vmicBox);
        startBtn_ = new QPushButton("Start loopback", vmicBox);
        stopBtn_ = new QPushButton("Stop loopback", vmicBox);

        buttonsRow->addWidget(createBtn_);
        buttonsRow->addWidget(destroyBtn_);
        buttonsRow->addSpacing(12);
        buttonsRow->addWidget(startBtn_);
        buttonsRow->addWidget(stopBtn_);
        buttonsRow->addStretch(1);

        vmicLayout->addLayout(buttonsRow);

        vmicLayout->addWidget(new QLabel(
            "Tip: In other apps, select “StudioCast Microphone”.\n"
            "Processed feed: the audio pipeline should play into “StudioCast Sink”, and apps should use\n"
            "“StudioCast Microphone” (sink monitor).\n"
            "This UI uses pactl (PipeWire-PulseAudio compatibility layer or PulseAudio).",
            vmicBox));

        root->addWidget(vmicBox);

        auto *speakersTitle = new QLabel("Speakers", this);
        speakersTitle->setStyleSheet("font-size: 20px; font-weight: 600;");
        root->addWidget(speakersTitle);

        // -----------------------
        // Virtual speakers controls
        // -----------------------
        auto *spkBox = new QGroupBox("StudioCast Speakers", this);
        auto *spkLayout = new QVBoxLayout(spkBox);

        auto *spkButtonsRow = new QHBoxLayout();
        enableSpeakersBtn_ = new QPushButton("Enable speakers device", spkBox);
        stopSpeakersBtn_ = new QPushButton("Stop routing", spkBox);
        destroySpeakersBtn_ = new QPushButton("Destroy speakers device", spkBox);

        spkButtonsRow->addWidget(enableSpeakersBtn_);
        spkButtonsRow->addWidget(stopSpeakersBtn_);
        spkButtonsRow->addWidget(destroySpeakersBtn_);
        spkButtonsRow->addStretch(1);
        spkLayout->addLayout(spkButtonsRow);

        spkLayout->addWidget(new QLabel(
            "Tip: In other apps, select “StudioCast Speakers” as the output device.\n"
            "StudioCast will route that audio to your physical speakers.",
            spkBox));

        root->addWidget(spkBox);

        // -----------------------
        // Status
        // -----------------------
        auto *statusBox = new QGroupBox("Status", this);
        auto *statusLayout = new QVBoxLayout(statusBox);

        statusText_ = new QPlainTextEdit(statusBox);
        statusText_->setReadOnly(true);
        statusText_->setMinimumHeight(220);
        statusLayout->addWidget(statusText_, 1);

        auto *statusButtonsRow = new QHBoxLayout();
        refreshStatusBtn_ = new QPushButton("Refresh status", statusBox);
        statusButtonsRow->addWidget(refreshStatusBtn_);
        statusButtonsRow->addStretch(1);
        statusLayout->addLayout(statusButtonsRow);

        root->addWidget(statusBox);
        root->addStretch(1);

        // -----------------------
        // Wiring
        // -----------------------
        connect(refreshSourcesBtn_, &QPushButton::clicked, this, &AudioPage::RefreshSources);
        connect(refreshStatusBtn_, &QPushButton::clicked, this, &AudioPage::RefreshStatus);

        connect(noiseRemovalCb_, &QCheckBox::toggled, this, &AudioPage::OnAiNoiseToggled);
        connect(echoRemovalCb_, &QCheckBox::toggled, this, &AudioPage::OnAiEchoToggled);
        connect(studioVoiceCb_, &QCheckBox::toggled, this, &AudioPage::OnAiStudioVoiceToggled);
        connect(strengthSlider_, &QSlider::valueChanged, this, &AudioPage::OnAiStrengthChanged);
        connect(aiStartBtn_, &QPushButton::clicked, this, &AudioPage::OnAiStart);
        connect(aiStopBtn_, &QPushButton::clicked, this, &AudioPage::OnAiStop);
        connect(aiRefreshBtn_, &QPushButton::clicked, this, &AudioPage::RefreshStatus);

        connect(createBtn_, &QPushButton::clicked, this, &AudioPage::OnCreateVirtualMic);
        connect(destroyBtn_, &QPushButton::clicked, this, &AudioPage::OnDestroyVirtualMic);
        connect(startBtn_, &QPushButton::clicked, this, &AudioPage::OnStartLoopback);
        connect(stopBtn_, &QPushButton::clicked, this, &AudioPage::OnStopLoopback);

        connect(enableSpeakersBtn_, &QPushButton::clicked, this, &AudioPage::OnEnableVirtualSpeakers);
        connect(stopSpeakersBtn_, &QPushButton::clicked, this, &AudioPage::OnStopSpeakersRouting);
        connect(destroySpeakersBtn_, &QPushButton::clicked, this, &AudioPage::OnDestroyVirtualSpeakers);

        connect(sourceCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &AudioPage::OnSourceChanged);

        // Poll status so the UI reflects external changes (user unloads modules, etc.)
        pollTimer_ = new QTimer(this);
        pollTimer_->setInterval(1500);
        connect(pollTimer_, &QTimer::timeout, this, &AudioPage::RefreshStatus);
        pollTimer_->start();

        RefreshSources();
        RefreshStatus();

#ifdef NDEBUG
        // In release/production builds we do not support pass-through routing via module-loopback.
        // The processed feed is expected to come from the audio pipeline (Maxine AFX -> studiocast_sink).
        inputBox->setTitle("Input (legacy loopback - disabled in release builds)");
        inputBox->setEnabled(false);
        startBtn_->setVisible(false);
        stopBtn_->setVisible(false);
#endif
    }

    void AudioPage::ShowError(const QString &title, const QString &details) {
        QMessageBox::critical(this, title, details);
    }

    void AudioPage::RefreshSources() {
        sourceCombo_->clear();
        portCombo_->clear();
        portCombo_->setEnabled(false);
        cachedSources_.clear();

        std::string pactlDetails;
        if (!studiocast::audio::pulse::PactlAvailable(&pactlDetails)) {
            sourceCombo_->addItem("pactl not available");
            sourceCombo_->setEnabled(false);
            ShowError("Audio", QString("pactl not available.\n\nDetails:\n%1")
                      .arg(QString::fromStdString(pactlDetails)));
            return;
        }

        sourceCombo_->setEnabled(true);

        std::string err;
        cachedSources_ = studiocast::audio::pulse::ListSourcesDetailed(&err);
        if (!err.empty()) {
            // Non-fatal warning, but useful
            statusText_->setPlainText(QString::fromStdString("Warning: " + err));
        }

        std::optional<std::string> defaultSource;
        {
            std::string derr;
            defaultSource = studiocast::audio::pulse::GetDefaultSourceName(&derr);
        }

        int defaultIndex = -1;
        int added = 0;

        for (const auto &s: cachedSources_) {
            if (s.name.empty()) continue;
            if (IsBadLoopbackSourceCandidate(s.name)) continue;

            const std::string label = s.description.empty() ? s.name : s.description;

            // Display label, but store raw source name in item data.
            sourceCombo_->addItem(QString::fromStdString(label),
                                  QVariant(QString::fromStdString(s.name)));

            if (defaultSource && s.name == *defaultSource) {
                defaultIndex = added;
            }
            ++added;
        }

        if (added == 0) {
            sourceCombo_->addItem("<no suitable sources found>");
            sourceCombo_->setEnabled(false);
            portCombo_->setEnabled(false);
            return;
        }

        sourceCombo_->setCurrentIndex(defaultIndex >= 0 ? defaultIndex : 0);
        OnSourceChanged(sourceCombo_->currentIndex());
    }

    void AudioPage::OnSourceChanged(int /*index*/) {
        portCombo_->clear();
        portCombo_->setEnabled(false);

        if (!sourceCombo_->isEnabled()) return;

        const std::string srcName = sourceCombo_->currentData().toString().toStdString();
        if (srcName.empty()) return;

        const studiocast::audio::pulse::PactlSourceInfo *info = nullptr;
        for (const auto &s: cachedSources_) {
            if (s.name == srcName) {
                info = &s;
                break;
            }
        }
        if (!info) return;

        if (info->ports.empty()) {
            // Many sources will have no explicit ports; that's fine.
            portCombo_->setEnabled(false);
            return;
        }

        portCombo_->setEnabled(true);

        int activeIdx = -1;
        int firstAvailable = -1;

        for (std::size_t i = 0; i < info->ports.size(); ++i) {
            const int idx = static_cast<int>(i);
            const auto& p = info->ports[i];

            std::string label = p.description.empty() ? p.name : p.description;
            if (!p.available) label += " (unavailable)";

            portCombo_->addItem(QString::fromStdString(label),
                                QVariant(QString::fromStdString(p.name)));

            if (!info->active_port.empty() && p.name == info->active_port) activeIdx = idx;
            if (firstAvailable < 0 && p.available) firstAvailable = idx;
        }

        if (activeIdx >= 0) portCombo_->setCurrentIndex(activeIdx);
        else if (firstAvailable >= 0) portCombo_->setCurrentIndex(firstAvailable);
        else portCombo_->setCurrentIndex(0);

        PushDaemonSourceSelection();
    }

    void AudioPage::RefreshStatus() {
        // Always show the detailed legacy status string, plus daemon status.
        RefreshDaemonAudioStatus();

        QString text = QString::fromStdString(studiocast::audio::StatusText());
        if (!daemonStatusText_.isEmpty()) {
            text += "\n\n---\nDaemon audio status:\n";
            text += daemonStatusText_;
        }
        statusText_->setPlainText(text);

        // Enable/disable buttons based on current loaded modules (best-effort).
        std::string pactlDetails;
        const bool pactlOk = studiocast::audio::pulse::PactlAvailable(&pactlDetails);
        createBtn_->setEnabled(pactlOk);
        destroyBtn_->setEnabled(pactlOk);
        startBtn_->setEnabled(pactlOk);
        stopBtn_->setEnabled(pactlOk);

        enableSpeakersBtn_->setEnabled(pactlOk);
        stopSpeakersBtn_->setEnabled(pactlOk);
        destroySpeakersBtn_->setEnabled(pactlOk);

        if (!pactlOk) return;

        std::string err;
        const auto mods = studiocast::audio::pulse::ListModules(&err);

        bool hasSink = false;
        bool hasRemap = false;
        bool hasLoopback = false;

        bool hasSpeakersSink = false;
        bool hasSpeakersLoopback = false;

        for (const auto &m: mods) {
            if (m.name == "module-null-sink" && Contains(m.args, "sink_name=studiocast_sink")) {
                hasSink = true;
            }
            if (m.name == "module-remap-source" && Contains(m.args, "source_name=studiocast_mic")) {
                hasRemap = true;
            }
            if (m.name == "module-loopback" && Contains(m.args, "sink=studiocast_sink")) {
                hasLoopback = true;
            }

            if (m.name == "module-null-sink" && Contains(m.args, "sink_name=studiocast_speakers")) {
                hasSpeakersSink = true;
            }
            if (m.name == "module-loopback" && Contains(m.args, "source=studiocast_speakers.monitor")) {
                hasSpeakersLoopback = true;
            }
        }

        // Create is always safe; disable Destroy if nothing exists.
        destroyBtn_->setEnabled(hasSink || hasRemap);
        startBtn_->setEnabled(hasSink && hasRemap);
        stopBtn_->setEnabled(hasLoopback);

        destroySpeakersBtn_->setEnabled(hasSpeakersSink);
        stopSpeakersBtn_->setEnabled(hasSpeakersLoopback);
    }

    void AudioPage::SetAiControlsEnabled(bool enabled, const QString& reason) {
        daemonAiSupported_ = enabled;
        daemonAiDisableReason_ = reason;

        noiseRemovalCb_->setEnabled(enabled);
        echoRemovalCb_->setEnabled(enabled);
        studioVoiceCb_->setEnabled(enabled);
        strengthSlider_->setEnabled(enabled);
        aiStartBtn_->setEnabled(enabled);
        aiStopBtn_->setEnabled(enabled);

        aiBanner_->setVisible(!enabled && !reason.isEmpty());
        aiBanner_->setText(reason);
    }

    void AudioPage::RefreshDaemonAudioStatus() {
        daemonStatusText_.clear();

        std::string json;
        QString err;
        if (!DaemonRequest("GET_STATUS", &json, &err)) {
            SetAiControlsEnabled(false, "Daemon unavailable: " + err);
            daemonStatusText_ = "daemon_unavailable: " + err;
            return;
        }

        QJsonObject root;
        QString jerr;
        if (!ParseJsonObject(json, &root, &jerr)) {
            SetAiControlsEnabled(false, "Daemon returned invalid JSON: " + jerr);
            daemonStatusText_ = "invalid_json";
            return;
        }

        const auto maxine = root.value("maxine").toObject();
        const bool supported = maxine.value("supported").toBool(false);
        const QString summary = maxine.value("summary").toString();
        const QString blockedReason = maxine.value("blocked_reason").toString();
        const auto blockedDetails = maxine.value("blocked_details").toArray();

        bool gpuOk = true;
        if (maxine.contains("gpu")) {
            gpuOk = maxine.value("gpu").toObject().value("ok").toBool(true);
        }

        bool afxOk = true;
        if (maxine.contains("afx")) {
            afxOk = maxine.value("afx").toObject().value("ok").toBool(true);
        } else if (maxine.contains("components")) {
            afxOk = maxine.value("components").toObject().value("afx").toObject().value("found").toBool(true);
        }

        QString disableReason;
        if (!supported || !gpuOk || !afxOk) {
            disableReason = "AI audio effects are not available on this system.";
            if (!summary.isEmpty()) disableReason += "\n\n" + summary;
            const auto english = FormatMaxineReasonCode(blockedReason);
            if (!english.isEmpty()) disableReason += "\n\nReason: " + english;
            if (!blockedDetails.isEmpty()) {
                disableReason += "\n\n";
                for (const auto& v : blockedDetails) {
                    disableReason += "- " + v.toString() + "\n";
                }
            }
        }

        SetAiControlsEnabled(disableReason.isEmpty(), disableReason);

        const auto audio = root.value("audio").toObject();
        const bool audioEnabled = audio.value("enabled").toBool(false);
        const QString micMode = audio.value("mic_mode").toString();
        const auto pipeline = audio.value("pipeline").toObject();
        const bool running = pipeline.value("running").toBool(false);
        const bool starting = pipeline.value("starting").toBool(false);
        const QString lastErr = pipeline.value("last_error").toString();

        daemonStatusText_ = QString("enabled=%1\nmic_mode=%2\npipeline=%3\n")
                                .arg(audioEnabled ? "true" : "false")
                                .arg(micMode.isEmpty() ? "(none)" : micMode)
                                .arg(running ? "running" : (starting ? "starting" : "stopped"));
        if (!lastErr.isEmpty()) daemonStatusText_ += "last_error: " + lastErr + "\n";

        if (!daemonAiSupported_) {
            // Keep widget states but prevent edits.
            return;
        }

        // Sync UI from daemon config.
        const auto fx = audio.value("audio_effects").toObject();
        const auto mic = fx.value("microphone").toObject();
        const bool noise = mic.value("noise_removal_enabled").toBool(false);
        const bool echo = mic.value("room_echo_removal_enabled").toBool(false);
        const bool studio = mic.value("studio_voice_enabled").toBool(false);
        const int strength = mic.value("strength").toInt(50);

        updatingAiUi_ = true;
        noiseRemovalCb_->setChecked(noise);
        echoRemovalCb_->setChecked(echo);
        studioVoiceCb_->setChecked(studio);
        strengthSlider_->setValue(std::max(0, std::min(100, strength)));
        strengthValueLabel_->setText(QString::number(strengthSlider_->value()));
        aiStartBtn_->setEnabled(!audioEnabled);
        aiStopBtn_->setEnabled(audioEnabled);
        updatingAiUi_ = false;
    }

    void AudioPage::PushDaemonSourceSelection() {
        if (!daemonAiSupported_) return;
        if (!sourceCombo_->isEnabled()) return;

        const std::string srcName = sourceCombo_->currentData().toString().toStdString();
        if (srcName.empty()) return;

        QJsonObject patch;
        patch.insert("source", QString::fromStdString(srcName));
        const auto json = QJsonDocument(patch).toJson(QJsonDocument::Compact);

        std::string out;
        QString err;
        if (!DaemonRequest(std::string("SET_AUDIO_CONFIG ") + json.toStdString(), &out, &err)) {
            // Non-fatal: show in status.
            daemonStatusText_ += "\nfailed_to_set_source: " + err;
        }
    }

    void AudioPage::PushDaemonAudioConfig() {
        if (!daemonAiSupported_) return;

        const bool studio = studioVoiceCb_->isChecked();
        const bool noise = !studio && noiseRemovalCb_->isChecked();
        const bool echo = !studio && echoRemovalCb_->isChecked();
        const int strength = std::max(0, std::min(100, strengthSlider_->value()));

        const bool enabled = (studio || noise || echo);

        QJsonObject mic;
        mic.insert("studio_voice_enabled", studio);
        mic.insert("noise_removal_enabled", noise);
        mic.insert("room_echo_removal_enabled", echo);
        mic.insert("strength", strength);

        QJsonObject spk;
        spk.insert("enabled", false);

        QJsonObject effects;
        effects.insert("schema_version", 1);
        effects.insert("microphone", mic);
        effects.insert("speaker", spk);

        QJsonObject patch;
        patch.insert("enabled", enabled);
        patch.insert("create_virtual_mic", true);
        patch.insert("audio_effects", effects);

        const auto json = QJsonDocument(patch).toJson(QJsonDocument::Compact);

        std::string out;
        QString err;
        if (!DaemonRequest(std::string("SET_AUDIO_CONFIG ") + json.toStdString(), &out, &err)) {
            ShowError("Audio", "Failed to update daemon audio config:\n\n" + err);
        }
        RefreshDaemonAudioStatus();
    }

    void AudioPage::OnAiNoiseToggled(bool checked) {
        if (updatingAiUi_) return;
        if (checked && studioVoiceCb_->isChecked()) {
            updatingAiUi_ = true;
            studioVoiceCb_->setChecked(false);
            updatingAiUi_ = false;
        }
        PushDaemonAudioConfig();
    }

    void AudioPage::OnAiEchoToggled(bool checked) {
        if (updatingAiUi_) return;
        if (checked && studioVoiceCb_->isChecked()) {
            updatingAiUi_ = true;
            studioVoiceCb_->setChecked(false);
            updatingAiUi_ = false;
        }
        PushDaemonAudioConfig();
    }

    void AudioPage::OnAiStudioVoiceToggled(bool checked) {
        if (updatingAiUi_) return;
        if (checked) {
            updatingAiUi_ = true;
            noiseRemovalCb_->setChecked(false);
            echoRemovalCb_->setChecked(false);
            updatingAiUi_ = false;
        }
        PushDaemonAudioConfig();
    }

    void AudioPage::OnAiStrengthChanged(int v) {
        strengthValueLabel_->setText(QString::number(std::max(0, std::min(100, v))));
        if (updatingAiUi_) return;
        PushDaemonAudioConfig();
    }

    void AudioPage::OnAiStart() {
        std::string out;
        QString err;
        if (!DaemonRequest("AUDIO_START", &out, &err)) {
            ShowError("Audio", "Failed to start daemon audio:\n\n" + err);
        }
        RefreshDaemonAudioStatus();
    }

    void AudioPage::OnAiStop() {
        std::string out;
        QString err;
        if (!DaemonRequest("AUDIO_STOP", &out, &err)) {
            ShowError("Audio", "Failed to stop daemon audio:\n\n" + err);
        }
        RefreshDaemonAudioStatus();
    }

    void AudioPage::OnCreateVirtualMic() {
        std::string err;
        if (!studiocast::audio::CreateVirtualMic(&err)) {
            ShowError("Create virtual mic failed", QString::fromStdString(err));
            return;
        }
        RefreshStatus();
        RefreshSources();
    }

    void AudioPage::OnDestroyVirtualMic() {
        std::string err;
        if (!studiocast::audio::DestroyVirtualMic(&err)) {
            ShowError("Destroy virtual mic failed", QString::fromStdString(err));
            return;
        }
        RefreshStatus();
        RefreshSources();
    }

    void AudioPage::OnStartLoopback() {
        if (!sourceCombo_->isEnabled()) {
            ShowError("Start loopback failed", "No valid input source selected.");
            return;
        }

        const auto selected = sourceCombo_->currentData().toString();
        const std::string source = selected.toStdString();
        const int latency = latencySpin_->value();

        // Optional: set port before loopback (helps laptop internal mic/headset mic routing).
        if (portCombo_ && portCombo_->isEnabled()) {
            const std::string port = portCombo_->currentData().toString().toStdString();
            if (!port.empty()) {
                std::string perr;
                if (!studiocast::audio::pulse::SetSourcePort(source, port, &perr)) {
                    ShowError("Set input port failed", QString::fromStdString(perr));
                    return;
                }
            }
        }

        std::string err;
        if (!studiocast::audio::StartLoopback(source, latency, &err)) {
            ShowError("Start loopback failed", QString::fromStdString(err));
            return;
        }
        RefreshStatus();
    }

    void AudioPage::OnStopLoopback() {
        std::string err;
        if (!studiocast::audio::StopLoopback(&err)) {
            ShowError("Stop loopback failed", QString::fromStdString(err));
            return;
        }
        RefreshStatus();
    }

    void AudioPage::OnEnableVirtualSpeakers() {
        std::string err;
        if (!studiocast::audio::CreateVirtualSpeaker(&err)) {
            ShowError("Enable speakers failed", QString::fromStdString(err));
            return;
        }

        err.clear();
        if (!studiocast::audio::StartSpeakerLoopback("", 10, &err)) {
            ShowError("Start speakers routing failed", QString::fromStdString(err));
            return;
        }

        RefreshStatus();
    }

    void AudioPage::OnStopSpeakersRouting() {
        std::string err;
        if (!studiocast::audio::StopSpeakerLoopback(&err)) {
            ShowError("Stop speakers routing failed", QString::fromStdString(err));
            return;
        }
        RefreshStatus();
    }

    void AudioPage::OnDestroyVirtualSpeakers() {
        std::string err;
        if (!studiocast::audio::DestroyVirtualSpeaker(&err)) {
            ShowError("Destroy speakers failed", QString::fromStdString(err));
            return;
        }
        RefreshStatus();
    }
} // namespace studiocast::gui
