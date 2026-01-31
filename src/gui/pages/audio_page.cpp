#include "audio_page.h"

#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <optional>
#include <string>
#include <vector>

#include "core/audio/pulse/pactl.h"
#include "core/audio/virtual_mic.h"

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
    } // namespace

    AudioPage::AudioPage(QWidget *parent) : QWidget(parent) {
        auto *root = new QVBoxLayout(this);
        root->setSpacing(12);

        auto *title = new QLabel("Microphone", this);
        title->setStyleSheet("font-size: 20px; font-weight: 600;");
        root->addWidget(title);

        // -----------------------
        // Input selection
        // -----------------------
        auto *inputBox = new QGroupBox("Input", this);
        auto *inputLayout = new QVBoxLayout(inputBox);

        auto *sourceRow = new QHBoxLayout();
        sourceRow->addWidget(new QLabel("Loopback source:", inputBox));

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
            "This uses pactl (PipeWire-PulseAudio compatibility layer or PulseAudio).",
            vmicBox));

        root->addWidget(vmicBox);

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

        connect(createBtn_, &QPushButton::clicked, this, &AudioPage::OnCreateVirtualMic);
        connect(destroyBtn_, &QPushButton::clicked, this, &AudioPage::OnDestroyVirtualMic);
        connect(startBtn_, &QPushButton::clicked, this, &AudioPage::OnStartLoopback);
        connect(stopBtn_, &QPushButton::clicked, this, &AudioPage::OnStopLoopback);

        connect(sourceCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &AudioPage::OnSourceChanged);

        // Poll status so the UI reflects external changes (user unloads modules, etc.)
        pollTimer_ = new QTimer(this);
        pollTimer_->setInterval(1500);
        connect(pollTimer_, &QTimer::timeout, this, &AudioPage::RefreshStatus);
        pollTimer_->start();

        RefreshSources();
        RefreshStatus();
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

        for (int i = 0; i < static_cast<int>(info->ports.size()); ++i) {
            const auto &p = info->ports[i];

            std::string label = p.description.empty() ? p.name : p.description;
            if (!p.available) label += " (unavailable)";

            portCombo_->addItem(QString::fromStdString(label),
                                QVariant(QString::fromStdString(p.name)));

            if (!info->active_port.empty() && p.name == info->active_port) activeIdx = i;
            if (firstAvailable < 0 && p.available) firstAvailable = i;
        }

        if (activeIdx >= 0) portCombo_->setCurrentIndex(activeIdx);
        else if (firstAvailable >= 0) portCombo_->setCurrentIndex(firstAvailable);
        else portCombo_->setCurrentIndex(0);
    }

    void AudioPage::RefreshStatus() {
        // Always show the detailed status string.
        statusText_->setPlainText(QString::fromStdString(studiocast::audio::StatusText()));

        // Enable/disable buttons based on current loaded modules (best-effort).
        std::string pactlDetails;
        const bool pactlOk = studiocast::audio::pulse::PactlAvailable(&pactlDetails);
        createBtn_->setEnabled(pactlOk);
        destroyBtn_->setEnabled(pactlOk);
        startBtn_->setEnabled(pactlOk);
        stopBtn_->setEnabled(pactlOk);

        if (!pactlOk) return;

        std::string err;
        const auto mods = studiocast::audio::pulse::ListModules(&err);

        bool hasSink = false;
        bool hasRemap = false;
        bool hasLoopback = false;

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
        }

        // Create is always safe; disable Destroy if nothing exists.
        destroyBtn_->setEnabled(hasSink || hasRemap);
        startBtn_->setEnabled(hasSink && hasRemap);
        stopBtn_->setEnabled(hasLoopback);
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
} // namespace studiocast::gui
