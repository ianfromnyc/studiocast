#include "video_page.h"

#include <QClipboard>
#include <QComboBox>
#include <QGuiApplication>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <sstream>

#include "core/video/v4l2_writer.h"
#include "core/video/v4l2loopback.h"

namespace studiocast::gui {
    namespace {
        QString DeviceLabel(const studiocast::video::VideoDevice &d) {
            QString label = QString::fromStdString(d.dev_node);
            if (!d.name.empty()) label += " — " + QString::fromStdString(d.name);
            if (d.is_loopback) label += " [loopback]";
            return label;
        }
    } // namespace

    VideoPage::VideoPage(QWidget *parent) : QWidget(parent) {
        auto *root = new QVBoxLayout(this);
        root->setSpacing(12);

        auto *title = new QLabel("Camera", this);
        title->setStyleSheet("font-size: 20px; font-weight: 600;");
        root->addWidget(title);

        auto *box = new QGroupBox("Virtual Camera (v4l2loopback)", this);
        auto *boxLayout = new QVBoxLayout(box);

        // Device row
        auto *devRow = new QHBoxLayout();
        devRow->addWidget(new QLabel("Output device:", box));

        deviceCombo_ = new QComboBox(box);
        devRow->addWidget(deviceCombo_, 1);

        refreshBtn_ = new QPushButton("Refresh", box);
        devRow->addWidget(refreshBtn_);

        boxLayout->addLayout(devRow);

        // Format row
        auto *fmtRow = new QHBoxLayout();
        fmtRow->addWidget(new QLabel("Format:", box));

        formatCombo_ = new QComboBox(box);
        formatCombo_->addItem("Auto (YUYV then RGB24)", "auto");
        formatCombo_->addItem("YUYV", "yuyv");
        formatCombo_->addItem("RGB24", "rgb24");
        fmtRow->addWidget(formatCombo_, 1);

        copyCmdBtn_ = new QPushButton("Copy modprobe command", box);
        fmtRow->addWidget(copyCmdBtn_);

        boxLayout->addLayout(fmtRow);

        // Size row
        auto *sizeRow = new QHBoxLayout();
        sizeRow->addWidget(new QLabel("Width:", box));
        widthSpin_ = new QSpinBox(box);
        widthSpin_->setRange(160, 3840);
        widthSpin_->setValue(1280);
        sizeRow->addWidget(widthSpin_);

        sizeRow->addWidget(new QLabel("Height:", box));
        heightSpin_ = new QSpinBox(box);
        heightSpin_->setRange(120, 2160);
        heightSpin_->setValue(720);
        sizeRow->addWidget(heightSpin_);

        sizeRow->addWidget(new QLabel("FPS:", box));
        fpsSpin_ = new QSpinBox(box);
        fpsSpin_->setRange(1, 120);
        fpsSpin_->setValue(30);
        sizeRow->addWidget(fpsSpin_);

        sizeRow->addStretch(1);
        boxLayout->addLayout(sizeRow);

        // Controls row
        auto *ctlRow = new QHBoxLayout();
        startBtn_ = new QPushButton("Start test pattern", box);
        stopBtn_ = new QPushButton("Stop", box);
        ctlRow->addWidget(startBtn_);
        ctlRow->addWidget(stopBtn_);
        ctlRow->addStretch(1);
        boxLayout->addLayout(ctlRow);

        // Status text
        statusText_ = new QPlainTextEdit(box);
        statusText_->setReadOnly(true);
        statusText_->setMinimumHeight(260);
        boxLayout->addWidget(statusText_, 1);

        root->addWidget(box);
        root->addStretch(1);

        connect(refreshBtn_, &QPushButton::clicked, this, &VideoPage::Refresh);
        connect(copyCmdBtn_, &QPushButton::clicked, this, &VideoPage::CopySuggestedCommand);
        connect(startBtn_, &QPushButton::clicked, this, &VideoPage::OnStartFeed);
        connect(stopBtn_, &QPushButton::clicked, this, &VideoPage::OnStopFeed);

        pollTimer_ = new QTimer(this);
        pollTimer_->setInterval(500);
        connect(pollTimer_, &QTimer::timeout, this, &VideoPage::OnPoll);
        pollTimer_->start();

        Refresh();
    }

    VideoPage::~VideoPage() {
        feed_.Stop();
    }

    void VideoPage::ShowError(const QString &title, const QString &details) {
        QMessageBox::critical(this, title, details);
    }

    void VideoPage::Refresh() {
        const auto rep = studiocast::video::ProbeLoopback();
        baseStatusText_ = rep.ToText();

        // Preserve selection if possible
        const QString prev = deviceCombo_->currentData().toString();
        deviceCombo_->clear();

        int setIndex = -1;
        int added = 0;
        for (const auto &d: rep.devices) {
            if (!d.is_loopback) continue;

            const QString label = DeviceLabel(d);
            const QString nodeData = QString::fromStdString(d.dev_node);
            deviceCombo_->addItem(label, nodeData);

            if (!prev.isEmpty() && prev == nodeData) setIndex = added;
            ++added;
        }

        if (added == 0) {
            deviceCombo_->addItem("<no v4l2loopback device found>", "");
            deviceCombo_->setEnabled(false);
        } else {
            deviceCombo_->setEnabled(true);
            deviceCombo_->setCurrentIndex(setIndex >= 0 ? setIndex : 0);
        }

        suggestedCmd_ = QString::fromStdString(rep.suggested_modprobe_cmd);
        copyCmdBtn_->setEnabled(!suggestedCmd_.isEmpty());

        UpdateStatusText();
        UpdateUiEnabled();
    }

    void VideoPage::CopySuggestedCommand() {
        if (suggestedCmd_.isEmpty()) return;
        if (auto *cb = QGuiApplication::clipboard()) {
            cb->setText(suggestedCmd_);
        }
    }

    void VideoPage::OnStartFeed() {
        const auto st = feed_.Status();
        if (st.running || st.starting) return;

        const QString dev = deviceCombo_->currentData().toString();
        if (dev.isEmpty()) {
            ShowError("Virtual Camera",
                      "No v4l2loopback device found.\n\nUse the modprobe command shown in the status box.");
            return;
        }

        studiocast::video::FeedConfig cfg;
        cfg.device = dev.toStdString();
        cfg.width = widthSpin_->value();
        cfg.height = heightSpin_->value();
        cfg.fps = fpsSpin_->value();

        const QString fmt = formatCombo_->currentData().toString();
        if (fmt == "yuyv") cfg.format = studiocast::video::FeedPixelFormatMode::yuyv;
        else if (fmt == "rgb24") cfg.format = studiocast::video::FeedPixelFormatMode::rgb24;
        else cfg.format = studiocast::video::FeedPixelFormatMode::auto_select;

        std::string err;
        if (!feed_.StartTestPattern(cfg, &err)) {
            ShowError("Start feed failed", QString::fromStdString(err));
            Refresh();
            return;
        }

        UpdateStatusText();
        UpdateUiEnabled();
    }

    void VideoPage::OnStopFeed() {
        feed_.Stop();
        UpdateStatusText();
        UpdateUiEnabled();
    }

    void VideoPage::OnPoll() {
        // Lightweight: do not re-probe devices every tick; just update feed status + enable states.
        UpdateStatusText();
        UpdateUiEnabled();
    }

    void VideoPage::UpdateUiEnabled() {
        const auto st = feed_.Status();
        const bool running = st.running || st.starting;

        startBtn_->setEnabled(
            !running && deviceCombo_->isEnabled() && !deviceCombo_->currentData().toString().isEmpty());
        stopBtn_->setEnabled(running);

        deviceCombo_->setEnabled(
            !running && deviceCombo_->count() > 0 && !deviceCombo_->itemData(0).toString().isEmpty());
        widthSpin_->setEnabled(!running);
        heightSpin_->setEnabled(!running);
        fpsSpin_->setEnabled(!running);
        formatCombo_->setEnabled(!running);
    }

    void VideoPage::UpdateStatusText() {
        const auto st = feed_.Status();

        std::ostringstream oss;
        oss << baseStatusText_;
        oss << "\n\n---\nFeed\n";

        if (st.starting) {
            oss << "  state: starting...\n";
        } else if (st.running) {
            oss << "  state: running\n";
        } else {
            oss << "  state: stopped\n";
        }

        if (!st.device.empty()) {
            oss << "  device: " << st.device << "\n";
        }

        if (st.running || st.starting) {
            oss << "  actual: " << st.actual.width << "x" << st.actual.height
                    << " @ " << st.actual.fps << " fps"
                    << " (" << studiocast::video::PixelFormatName(st.actual.format) << ")\n";
            oss << "  frames: " << st.frame_index << "\n";
        }

        if (!st.last_error.empty()) {
            oss << "  last error: " << st.last_error << "\n";
        }

        statusText_->setPlainText(QString::fromStdString(oss.str()));
    }
} // namespace studiocast::gui
