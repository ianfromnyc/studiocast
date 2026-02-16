#include "audio_page.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QFileDialog>
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
        auto* aiBox = new QGroupBox("AI microphone effects (daemon)", this);
        auto* aiLayout = new QVBoxLayout(aiBox);

        // Backend selection (mirrors video_page.cpp)
        auto* engineRow = new QHBoxLayout();
        engineRow->addWidget(new QLabel("Effect engine:", aiBox));
        engineCombo_ = new QComboBox(aiBox);
        engineCombo_->addItem("Auto", "auto");
        engineCombo_->addItem("Maxine", "maxine");
        engineCombo_->addItem("Open Source", "open_source");
        engineCombo_->addItem("Off", "off");
        engineRow->addWidget(engineCombo_);
        engineRow->addSpacing(12);
        engineRow->addWidget(new QLabel("Active:", aiBox));
        engineActiveValue_ = new QLabel("—", aiBox);
        engineActiveValue_->setStyleSheet("font-weight: 600;");
        engineRow->addWidget(engineActiveValue_);
        engineRow->addStretch(1);
        aiLayout->addLayout(engineRow);

        aiInfoBanner_ = new QLabel(aiBox);
        aiInfoBanner_->setWordWrap(true);
        aiInfoBanner_->setStyleSheet(
            "background: #14213a; border: 1px solid #334466; color: #d0e0f0; padding: 8px; border-radius: 4px;");
        aiInfoBanner_->setVisible(false);
        aiLayout->addWidget(aiInfoBanner_);

        aiBanner_ = new QLabel(aiBox);
        aiBanner_->setWordWrap(true);
        aiBanner_->setStyleSheet(
            "background: #3a1414; border: 1px solid #663333; color: #f0d0d0; padding: 8px; border-radius: 4px;");
        aiBanner_->setVisible(false);
        aiLayout->addWidget(aiBanner_);

        // Open-source model selection (Open Audio packs)
        auto* modelRow = new QHBoxLayout();
        openAudioModelLabel_ = new QLabel("Open-source model:", aiBox);
        modelRow->addWidget(openAudioModelLabel_);
        openAudioModelCombo_ = new QComboBox(aiBox);
        openAudioModelCombo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        openAudioModelCombo_->addItem("Default (auto)", "");
        modelRow->addWidget(openAudioModelCombo_, 1);
        aiLayout->addLayout(modelRow);

        auto* modelPathRow = new QHBoxLayout();
        openAudioModelPathLabel_ = new QLabel("Model path (optional):", aiBox);
        modelPathRow->addWidget(openAudioModelPathLabel_);
        openAudioModelPathEdit_ = new QLineEdit(aiBox);
        openAudioModelPathEdit_->setPlaceholderText("/path/to/model.onnx or /path/to/pack/");
        modelPathRow->addWidget(openAudioModelPathEdit_, 1);
        browseOpenAudioModelBtn_ = new QPushButton("Browse…", aiBox);
        modelPathRow->addWidget(browseOpenAudioModelBtn_);
        aiLayout->addLayout(modelPathRow);

        openAudioInstallHintsBtn_ = new QPushButton("Open Audio install hints", aiBox);
        openAudioInstallHintsBtn_->setEnabled(false);
        aiLayout->addWidget(openAudioInstallHintsBtn_, 0, Qt::AlignLeft);

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

        UpdateMicInterlocks();

        UpdateEngineUiVisibility();

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
        // AI effects (daemon-driven)
        // -----------------------
        auto* aiSpkBox = new QGroupBox("AI speaker effects (Maxine AFX via daemon)", this);
        auto* aiSpkLayout = new QVBoxLayout(aiSpkBox);

        auto* aiSpkRow = new QHBoxLayout();
        speakerNoiseRemovalCb_ = new QCheckBox("Noise removal", aiSpkBox);
        aiSpkRow->addWidget(speakerNoiseRemovalCb_);
        aiSpkRow->addStretch(1);
        aiSpkLayout->addLayout(aiSpkRow);

        auto* spkStrengthRow = new QHBoxLayout();
        spkStrengthRow->addWidget(new QLabel("Strength:", aiSpkBox));
        speakerStrengthSlider_ = new QSlider(Qt::Horizontal, aiSpkBox);
        speakerStrengthSlider_->setRange(0, 100);
        speakerStrengthSlider_->setValue(50);
        spkStrengthRow->addWidget(speakerStrengthSlider_, 1);
        speakerStrengthValueLabel_ = new QLabel("50", aiSpkBox);
        speakerStrengthValueLabel_->setMinimumWidth(32);
        speakerStrengthValueLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        spkStrengthRow->addWidget(speakerStrengthValueLabel_);
        aiSpkLayout->addLayout(spkStrengthRow);

        aiSpkLayout->addWidget(new QLabel(
            "Applies noise removal to speaker output (where supported).",
            aiSpkBox));

        root->addWidget(aiSpkBox);

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

        connect(engineCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &AudioPage::OnAiEngineChanged);
        connect(openAudioModelCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &AudioPage::OnAiOpenAudioModelChanged);
        connect(openAudioModelPathEdit_, &QLineEdit::editingFinished, this, &AudioPage::OnAiOpenAudioModelPathEdited);
        connect(browseOpenAudioModelBtn_, &QPushButton::clicked, this, &AudioPage::OnAiBrowseOpenAudioModel);
        connect(openAudioInstallHintsBtn_, &QPushButton::clicked, this, &AudioPage::OnOpenAudioInstallHints);
        connect(speakerNoiseRemovalCb_, &QCheckBox::toggled, this, &AudioPage::OnAiSpeakerNoiseToggled);
        connect(speakerStrengthSlider_, &QSlider::valueChanged, this, &AudioPage::OnAiSpeakerStrengthChanged);
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
        stopSpeakersBtn_->setEnabled(hasSpeakersLoopback || daemonSpeakersRoutingActive_);
    }

    void AudioPage::SetAiControlsEnabled(bool enabled, const QString& reason) {
        daemonAiSupported_ = enabled;
        daemonAiDisableReason_ = reason;

        if (engineCombo_) engineCombo_->setEnabled(enabled);

        if (openAudioModelCombo_) openAudioModelCombo_->setEnabled(enabled);
        if (openAudioModelPathEdit_) openAudioModelPathEdit_->setEnabled(enabled);
        if (browseOpenAudioModelBtn_) browseOpenAudioModelBtn_->setEnabled(enabled);

        noiseRemovalCb_->setEnabled(enabled);
        echoRemovalCb_->setEnabled(enabled);
        studioVoiceCb_->setEnabled(enabled);
        strengthSlider_->setEnabled(enabled);
        strengthValueLabel_->setEnabled(enabled);

        speakerNoiseRemovalCb_->setEnabled(enabled);
        speakerStrengthSlider_->setEnabled(enabled);
        speakerStrengthValueLabel_->setEnabled(enabled);
        aiStartBtn_->setEnabled(enabled);
        aiStopBtn_->setEnabled(enabled);

        aiBanner_->setVisible(!enabled && !reason.isEmpty());
        aiBanner_->setText(reason);

        UpdateMicInterlocks();
    }

    void AudioPage::UpdateEngineUiVisibility() {
        const QString eng = engineCombo_ ? engineCombo_->currentData().toString() : QStringLiteral("auto");
        const bool showOpen = (eng == "open_source" || eng == "auto");

        if (openAudioModelLabel_) openAudioModelLabel_->setVisible(showOpen);
        if (openAudioModelCombo_) openAudioModelCombo_->setVisible(showOpen);
        if (openAudioModelPathLabel_) openAudioModelPathLabel_->setVisible(showOpen);
        if (openAudioModelPathEdit_) openAudioModelPathEdit_->setVisible(showOpen);
        if (browseOpenAudioModelBtn_) browseOpenAudioModelBtn_->setVisible(showOpen);
        if (openAudioInstallHintsBtn_) openAudioInstallHintsBtn_->setVisible(showOpen);
    }

    void AudioPage::UpdateMicInterlocks() {
        if (!noiseRemovalCb_ || !echoRemovalCb_ || !studioVoiceCb_ || !strengthSlider_) return;

        const bool studio = studioVoiceCb_->isChecked();
        const bool allowNoiseEcho = daemonAiSupported_ && !studio;
        noiseRemovalCb_->setEnabled(allowNoiseEcho);
        echoRemovalCb_->setEnabled(allowNoiseEcho);
        strengthSlider_->setEnabled(allowNoiseEcho);
        if (strengthValueLabel_) strengthValueLabel_->setEnabled(allowNoiseEcho);
    }

    void AudioPage::RefreshDaemonAudioStatus() {
        daemonStatusText_.clear();
        daemonSpeakersRoutingActive_ = false;
        daemonSpeakersRouteMode_.clear();

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

        // Keep Maxine diagnostics for user guidance, but do not hard-disable the UI.
        // The runtime resolver handles Maxine/OpenAudio selection and fallback.
        QString maxineDiag;
        {
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

            if (!supported || !gpuOk || !afxOk) {
                maxineDiag = "maxine_unavailable: true\n";
                if (!summary.isEmpty()) maxineDiag += "maxine_summary: " + summary + "\n";
                const auto english = FormatMaxineReasonCode(blockedReason);
                if (!english.isEmpty()) maxineDiag += "maxine_reason: " + english + "\n";
                if (!blockedDetails.isEmpty()) {
                    maxineDiag += "maxine_details:\n";
                    for (const auto& v : blockedDetails) {
                        maxineDiag += "- " + v.toString() + "\n";
                    }
                }
            }
        }

        SetAiControlsEnabled(true, "");

        const auto audio = root.value("audio").toObject();
        const bool audioEnabled = audio.value("enabled").toBool(false);
        const QString micMode = audio.value("mic_mode").toString();
        const auto pipeline = audio.value("pipeline").toObject();
        const bool running = pipeline.value("running").toBool(false);
        const bool starting = pipeline.value("starting").toBool(false);
        const QString lastErr = pipeline.value("last_error").toString();

        const QString backendActive = pipeline.value("backend_active").toString();
        const QString effectsNote = pipeline.value("effects_note").toString();

        if (engineActiveValue_) {
            engineActiveValue_->setText(backendActive.isEmpty() ? QStringLiteral("—") : backendActive);
        }

        if (aiInfoBanner_) {
            const QString note = effectsNote.trimmed();
            aiInfoBanner_->setVisible(!note.isEmpty());
            aiInfoBanner_->setText(note);
        }

        daemonStatusText_ = QString("enabled=%1\nmic_mode=%2\npipeline=%3\n")
                                .arg(audioEnabled ? "true" : "false")
                                .arg(micMode.isEmpty() ? "(none)" : micMode)
                                .arg(running ? "running" : (starting ? "starting" : "stopped"));

        if (pipeline.contains("gpu")) {
            const auto gpu = pipeline.value("gpu").toObject();
            const int idx = gpu.value("index").toInt(-1);
            const QString name = gpu.value("name").toString();
            const QString cc = gpu.value("compute_cap").toString();
            if (idx >= 0 || !name.isEmpty()) {
                daemonStatusText_ += QString("gpu: #%1 %2%3\n")
                                        .arg(idx)
                                        .arg(name.isEmpty() ? "(unknown)" : name)
                                        .arg(cc.isEmpty() ? "" : (" (cc " + cc + ")"));
            }
        }
        if (!lastErr.isEmpty()) daemonStatusText_ += "last_error: " + lastErr + "\n";

        if (pipeline.contains("process_ms_avg")) {
            const double avgMs = pipeline.value("process_ms_avg").toDouble(0.0);
            const double lastMs = pipeline.value("process_us_last").toDouble(0.0) / 1000.0;
            const double maxMs = pipeline.value("process_us_max").toDouble(0.0) / 1000.0;
            const int overruns = pipeline.value("process_overruns").toInt(0);
            const qint64 frames = static_cast<qint64>(pipeline.value("frames_processed").toDouble(0.0));
            daemonStatusText_ += QString("proc_ms_avg=%1 proc_ms_last=%2 proc_ms_max=%3 overruns=%4 frames=%5\n")
                                    .arg(avgMs, 0, 'f', 3)
                                    .arg(lastMs, 0, 'f', 3)
                                    .arg(maxMs, 0, 'f', 3)
                                    .arg(overruns)
                                    .arg(frames);
        }

        // Speakers status (daemon-managed routing).
        if (audio.contains("speakers")) {
            const auto spk = audio.value("speakers").toObject();
            const bool spkPresent = spk.value("present").toBool(false);
            const bool spkRouting = spk.value("routing_active").toBool(false);
            const QString spkRouteMode = spk.value("route_mode").toString();

            // Cache for button logic (routing may be provided by the daemon pipeline,
            // not by module-loopback).
            daemonSpeakersRoutingActive_ = spkRouting;
            daemonSpeakersRouteMode_ = spkRouteMode;
            const bool spkPipeRunning = spk.value("pipeline_running").toBool(false);
            const bool spkPipeStarting = spk.value("pipeline_starting").toBool(false);
            const QString spkBackend = spk.value("backend_active").toString();
            const QString spkNote = spk.value("effects_note").toString();
            const QString spkTarget = spk.value("target_sink_active").toString();
            const QString spkErr = spk.value("last_error").toString();
            const QString spkPipeErr = spk.value("pipeline_last_error").toString();

            daemonStatusText_ += QString("speakers_present=%1\n").arg(spkPresent ? "true" : "false");
            daemonStatusText_ += QString("speakers_routing=%1\n").arg(spkRouting ? "active" : "off");
            if (!spkRouteMode.isEmpty()) daemonStatusText_ += "speakers_route_mode: " + spkRouteMode + "\n";
            if (spkPipeRunning || spkPipeStarting) {
                daemonStatusText_ += QString("speakers_pipeline=%1\n")
                                        .arg(spkPipeRunning ? "running" : (spkPipeStarting ? "starting" : "stopped"));
            }
            if (!spkBackend.isEmpty()) daemonStatusText_ += "speakers_backend_active: " + spkBackend + "\n";
            if (!spkNote.trimmed().isEmpty()) daemonStatusText_ += "speakers_note: " + spkNote.trimmed() + "\n";
            if (!spkTarget.isEmpty()) daemonStatusText_ += "speakers_target_sink_active: " + spkTarget + "\n";
            if (!spkErr.isEmpty()) daemonStatusText_ += "speakers_last_error: " + spkErr + "\n";
            if (!spkPipeErr.isEmpty()) daemonStatusText_ += "speakers_pipeline_last_error: " + spkPipeErr + "\n";

            if (spk.contains("pipeline_perf")) {
                const auto perf = spk.value("pipeline_perf").toObject();
                const double avgMs = perf.value("process_ms_avg").toDouble(0.0);
                const double lastMs = perf.value("process_us_last").toDouble(0.0) / 1000.0;
                const double maxMs = perf.value("process_us_max").toDouble(0.0) / 1000.0;
                const int overruns = perf.value("process_overruns").toInt(0);
                const qint64 frames = static_cast<qint64>(perf.value("frames_processed").toDouble(0.0));
                daemonStatusText_ += QString("speakers_proc_ms_avg=%1 speakers_proc_ms_last=%2 speakers_proc_ms_max=%3 speakers_overruns=%4 speakers_frames=%5\n")
                                        .arg(avgMs, 0, 'f', 3)
                                        .arg(lastMs, 0, 'f', 3)
                                        .arg(maxMs, 0, 'f', 3)
                                        .arg(overruns)
                                        .arg(frames);
            }
        }

        if (!backendActive.isEmpty()) daemonStatusText_ += "backend_active: " + backendActive + "\n";
        if (!maxineDiag.isEmpty()) daemonStatusText_ += maxineDiag;

        if (!daemonAiSupported_) {
            // Keep widget states but prevent edits.
            return;
        }

        // Sync UI from daemon config.
        const auto fx = audio.value("audio_effects").toObject();
        lastAudioEffectsObj_ = fx;

        // Engine preference (schema v3).
        const QString engine = fx.value("engine").toString("auto");
        if (engineCombo_) {
            engineCombo_->blockSignals(true);
            const int idx = engineCombo_->findData(engine);
            engineCombo_->setCurrentIndex(idx >= 0 ? idx : 0);
            engineCombo_->blockSignals(false);
        }

        // Open Audio diagnostics.
        openAudioStatusPresent_ = false;
        openAudioOk_ = false;
        openAudioInstallHints_.clear();

        QJsonObject openAudio;
        if (root.contains("engines")) {
            const auto engines = root.value("engines").toObject();
            openAudio = engines.value("open_audio").toObject();
        }
        if (openAudio.isEmpty()) {
            openAudio = root.value("open_audio").toObject();
        }
        if (!openAudio.isEmpty()) {
            openAudioStatusPresent_ = true;
            openAudioOk_ = openAudio.value("ok").toBool(false);
            const auto hints = openAudio.value("install_hints").toArray();
            for (const auto& v : hints) {
                const auto s = v.toString();
                if (!s.isEmpty()) openAudioInstallHints_.push_back(s);
            }
        }

        if (openAudioStatusPresent_) {
            daemonStatusText_ += QString("open_audio_ok: %1\n").arg(openAudioOk_ ? "true" : "false");
        }

        if (openAudioInstallHintsBtn_) {
            openAudioInstallHintsBtn_->setEnabled(openAudioStatusPresent_ && !openAudioInstallHints_.isEmpty());
        }

        // Populate model list from Open Audio diagnostics.
        if (openAudioModelCombo_) {
            const QString prior = openAudioModelCombo_->currentData().toString();
            openAudioModelCombo_->blockSignals(true);
            openAudioModelCombo_->clear();
            openAudioModelCombo_->addItem("Default (auto)", "");
            if (!openAudioStatusPresent_) {
                openAudioModelCombo_->addItem("<Open Audio status not reported>", "");
            } else {
                const auto models = openAudio.value("models").toArray();
                for (const auto& mv : models) {
                    const auto m = mv.toObject();
                    const QString id = m.value("id").toString();
                    const QString name = m.value("display_name").toString();
                    if (id.isEmpty()) continue;
                    const QString label = name.isEmpty() ? id : (name + " (" + id + ")");
                    openAudioModelCombo_->addItem(label, id);
                }
            }

            // Restore selection if possible.
            int restore = openAudioModelCombo_->findData(prior);
            if (restore < 0) restore = 0;
            openAudioModelCombo_->setCurrentIndex(restore);
            openAudioModelCombo_->blockSignals(false);
        }
        const auto mic = fx.value("microphone").toObject();
        const bool noise = mic.value("noise_removal_enabled").toBool(false);
        const bool echo = mic.value("room_echo_removal_enabled").toBool(false);
        const bool studio = mic.value("studio_voice_enabled").toBool(false);
        const int strength = mic.value("strength").toInt(50);

        const QString micModelId = mic.value("model_id").toString();
        const QString micModelPath = mic.value("model_path").toString();

        const auto spk = fx.value("speaker").toObject();
        const bool spkNoise = spk.value("noise_removal_enabled").toBool(false);
        const int spkStrength = spk.value("strength").toInt(50);

        updatingAiUi_ = true;
        noiseRemovalCb_->setChecked(noise);
        echoRemovalCb_->setChecked(echo);
        studioVoiceCb_->setChecked(studio);
        strengthSlider_->setValue(std::max(0, std::min(100, strength)));
        strengthValueLabel_->setText(QString::number(strengthSlider_->value()));

        if (openAudioModelCombo_) {
            const int idx = openAudioModelCombo_->findData(micModelId);
            if (idx >= 0) openAudioModelCombo_->setCurrentIndex(idx);
        }
        if (openAudioModelPathEdit_) {
            openAudioModelPathEdit_->setText(micModelPath);
        }

        speakerNoiseRemovalCb_->setChecked(spkNoise);
        speakerStrengthSlider_->setValue(std::max(0, std::min(100, spkStrength)));
        speakerStrengthValueLabel_->setText(QString::number(speakerStrengthSlider_->value()));

        aiStartBtn_->setEnabled(!audioEnabled);
        aiStopBtn_->setEnabled(audioEnabled);
        updatingAiUi_ = false;

        UpdateMicInterlocks();
        UpdateEngineUiVisibility();
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

        const QString engine = engineCombo_ ? engineCombo_->currentData().toString() : QStringLiteral("auto");

        QJsonObject effects = lastAudioEffectsObj_;
        effects.insert("schema_version", 3);
        effects.insert("engine", engine);

        QJsonObject mic = effects.value("microphone").toObject();
        mic.insert("studio_voice_enabled", studio);
        mic.insert("noise_removal_enabled", noise);
        mic.insert("room_echo_removal_enabled", echo);
        mic.insert("strength", strength);
        if (openAudioModelCombo_) mic.insert("model_id", openAudioModelCombo_->currentData().toString());
        if (openAudioModelPathEdit_) mic.insert("model_path", openAudioModelPathEdit_->text().trimmed());
        effects.insert("microphone", mic);

        const bool spkNoise = speakerNoiseRemovalCb_ && speakerNoiseRemovalCb_->isChecked();
        const int spkStrength = speakerStrengthSlider_ ? std::max(0, std::min(100, speakerStrengthSlider_->value())) : 50;

        QJsonObject spk = effects.value("speaker").toObject();
        spk.insert("noise_removal_enabled", spkNoise);
        spk.insert("strength", spkStrength);
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

    void AudioPage::OnAiEngineChanged(int /*index*/) {
        if (updatingAiUi_) return;
        UpdateEngineUiVisibility();
        PushDaemonAudioConfig();
    }

    void AudioPage::OnAiOpenAudioModelChanged(int /*index*/) {
        if (updatingAiUi_) return;
        PushDaemonAudioConfig();
    }

    void AudioPage::OnAiOpenAudioModelPathEdited() {
        if (updatingAiUi_) return;
        PushDaemonAudioConfig();
    }

    void AudioPage::OnAiBrowseOpenAudioModel() {
        if (!openAudioModelPathEdit_) return;
        const QString start = openAudioModelPathEdit_->text().trimmed();
        const QString path = QFileDialog::getOpenFileName(this, "Select ONNX model", start,
                                                         "ONNX model (*.onnx);;All files (*)");
        if (path.isEmpty()) return;
        openAudioModelPathEdit_->setText(path);
        if (updatingAiUi_) return;
        PushDaemonAudioConfig();
    }

    void AudioPage::OnOpenAudioInstallHints() {
        if (openAudioInstallHints_.isEmpty()) {
            ShowError("Open Audio", "No install hints were reported by the daemon.");
            return;
        }
        QString msg;
        for (const auto& s : openAudioInstallHints_) {
            msg += "- " + s + "\n";
        }
        QMessageBox::information(this, "Open Audio install hints", msg);
    }

    void AudioPage::OnAiNoiseToggled(bool checked) {
        if (updatingAiUi_) return;
        if (checked && studioVoiceCb_->isChecked()) {
            updatingAiUi_ = true;
            studioVoiceCb_->setChecked(false);
            updatingAiUi_ = false;
        }
        UpdateMicInterlocks();
        PushDaemonAudioConfig();
    }

    void AudioPage::OnAiEchoToggled(bool checked) {
        if (updatingAiUi_) return;
        if (checked && studioVoiceCb_->isChecked()) {
            updatingAiUi_ = true;
            studioVoiceCb_->setChecked(false);
            updatingAiUi_ = false;
        }
        UpdateMicInterlocks();
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
        UpdateMicInterlocks();
        PushDaemonAudioConfig();
    }

    void AudioPage::OnAiSpeakerNoiseToggled(bool /*checked*/) {
        if (updatingAiUi_) return;
        PushDaemonAudioConfig();
    }

    void AudioPage::OnAiSpeakerStrengthChanged(int v) {
        if (speakerStrengthValueLabel_) {
            speakerStrengthValueLabel_->setText(QString::number(std::max(0, std::min(100, v))));
        }
        if (updatingAiUi_) return;
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
        // Preferred path: let the daemon manage the virtual speakers device + routing.
        if (daemonAiSupported_) {
            QJsonObject patch;
            patch.insert("create_virtual_speakers", true);
            patch.insert("speakers_enabled", true);
            patch.insert("speaker_latency_ms", 10);

            const auto json = QJsonDocument(patch).toJson(QJsonDocument::Compact);
            std::string out;
            QString err;
            if (!DaemonRequest(std::string("SET_AUDIO_CONFIG ") + json.toStdString(), &out, &err)) {
                ShowError("Audio", "Failed to enable speakers via daemon:\n\n" + err);
                return;
            }
            RefreshStatus();
            return;
        }

        // Fallback: direct pactl manipulation (debug/dev only).
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
        if (daemonAiSupported_) {
            QJsonObject patch;
            patch.insert("speakers_enabled", false);
            const auto json = QJsonDocument(patch).toJson(QJsonDocument::Compact);
            std::string out;
            QString err;
            if (!DaemonRequest(std::string("SET_AUDIO_CONFIG ") + json.toStdString(), &out, &err)) {
                ShowError("Audio", "Failed to stop speakers routing via daemon:\n\n" + err);
                return;
            }
            RefreshStatus();
            return;
        }

        std::string err;
        if (!studiocast::audio::StopSpeakerLoopback(&err)) {
            ShowError("Stop speakers routing failed", QString::fromStdString(err));
            return;
        }
        RefreshStatus();
    }

    void AudioPage::OnDestroyVirtualSpeakers() {
        if (daemonAiSupported_) {
            QJsonObject patch;
            patch.insert("speakers_enabled", false);
            patch.insert("create_virtual_speakers", false);
            const auto json = QJsonDocument(patch).toJson(QJsonDocument::Compact);
            std::string out;
            QString err;
            if (!DaemonRequest(std::string("SET_AUDIO_CONFIG ") + json.toStdString(), &out, &err)) {
                ShowError("Audio", "Failed to destroy speakers via daemon:\n\n" + err);
                return;
            }
            RefreshStatus();
            return;
        }

        std::string err;
        if (!studiocast::audio::DestroyVirtualSpeaker(&err)) {
            ShowError("Destroy speakers failed", QString::fromStdString(err));
            return;
        }
        RefreshStatus();
    }
} // namespace studiocast::gui
