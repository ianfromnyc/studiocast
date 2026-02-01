#include "video_page.h"

#include <QCheckBox>
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

#include "core/video/v4l2loopback.h"
#include "core/video/v4l2_writer.h"

namespace studiocast::gui {
namespace {

QString DeviceLabel(const studiocast::video::VideoDevice& d) {
  QString label = QString::fromStdString(d.dev_node);
  if (!d.name.empty()) label += " — " + QString::fromStdString(d.name);
  if (!d.driver.empty()) label += " (" + QString::fromStdString(d.driver) + ")";
  if (d.is_loopback) label += " [loopback]";
  return label;
}

}  // namespace

VideoPage::VideoPage(QWidget* parent) : QWidget(parent) {
  auto* root = new QVBoxLayout(this);
  root->setSpacing(12);

  auto* title = new QLabel("Camera", this);
  title->setStyleSheet("font-size: 20px; font-weight: 600;");
  root->addWidget(title);

  auto* box = new QGroupBox("Processed Camera → Virtual Camera", this);
  auto* boxLayout = new QVBoxLayout(box);

  // Input row
  auto* inRow = new QHBoxLayout();
  inRow->addWidget(new QLabel("Input camera:", box));
  inputCombo_ = new QComboBox(box);
  inRow->addWidget(inputCombo_, 1);
  refreshBtn_ = new QPushButton("Refresh", box);
  inRow->addWidget(refreshBtn_);
  boxLayout->addLayout(inRow);

  // Output row
  auto* outRow = new QHBoxLayout();
  outRow->addWidget(new QLabel("Output (v4l2loopback):", box));
  outputCombo_ = new QComboBox(box);
  outRow->addWidget(outputCombo_, 1);
  copyCmdBtn_ = new QPushButton("Copy modprobe command", box);
  outRow->addWidget(copyCmdBtn_);
  boxLayout->addLayout(outRow);

  // Size row
  auto* sizeRow = new QHBoxLayout();
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

  // Effects row
  auto* fxRow = new QHBoxLayout();
  mirrorCheck_ = new QCheckBox("Mirror (horizontal flip)", box);
  fxRow->addWidget(mirrorCheck_);
  fxRow->addStretch(1);
  boxLayout->addLayout(fxRow);

  // Controls row
  auto* ctlRow = new QHBoxLayout();
  startBtn_ = new QPushButton("Start", box);
  stopBtn_ = new QPushButton("Stop", box);
  ctlRow->addWidget(startBtn_);
  ctlRow->addWidget(stopBtn_);
  ctlRow->addStretch(1);
  boxLayout->addLayout(ctlRow);

  // Status
  statusText_ = new QPlainTextEdit(box);
  statusText_->setReadOnly(true);
  statusText_->setMinimumHeight(280);
  boxLayout->addWidget(statusText_, 1);

  root->addWidget(box);
  root->addStretch(1);

  connect(refreshBtn_, &QPushButton::clicked, this, &VideoPage::Refresh);
  connect(copyCmdBtn_, &QPushButton::clicked, this, &VideoPage::CopySuggestedCommand);
  connect(startBtn_, &QPushButton::clicked, this, &VideoPage::OnStart);
  connect(stopBtn_, &QPushButton::clicked, this, &VideoPage::OnStop);
  connect(mirrorCheck_, &QCheckBox::toggled, this, &VideoPage::OnMirrorToggled);

  pollTimer_ = new QTimer(this);
  pollTimer_->setInterval(500);
  connect(pollTimer_, &QTimer::timeout, this, &VideoPage::OnPoll);
  pollTimer_->start();

  Refresh();
  UpdateUiEnabled();
}

VideoPage::~VideoPage() {
  pipeline_.Stop();
}

void VideoPage::ShowError(const QString& title, const QString& details) {
  QMessageBox::critical(this, title, details);
}

void VideoPage::Refresh() {
  const auto rep = studiocast::video::ProbeLoopback();
  baseStatusText_ = rep.ToText();

  // Preserve selection
  const QString prevIn = inputCombo_->currentData().toString();
  const QString prevOut = outputCombo_->currentData().toString();

  inputCombo_->clear();
  outputCombo_->clear();

  int inSet = -1;
  int outSet = -1;

  int inAdded = 0;
  int outAdded = 0;

  for (const auto& d : rep.devices) {
    const QString deviceNode = QString::fromStdString(d.dev_node);
    const QString label = DeviceLabel(d);

    if (!d.is_loopback && d.can_read) {
      inputCombo_->addItem(label, deviceNode);
      if (!prevIn.isEmpty() && prevIn == deviceNode) inSet = inAdded;
      ++inAdded;
    }

    if (d.is_loopback && d.can_write) {
      outputCombo_->addItem(label, deviceNode);
      if (!prevOut.isEmpty() && prevOut == deviceNode) outSet = outAdded;
      ++outAdded;
    }
  }

  if (inAdded == 0) {
    inputCombo_->addItem("<no readable camera found>", "");
    inputCombo_->setEnabled(false);
  } else {
    inputCombo_->setEnabled(true);
    inputCombo_->setCurrentIndex(inSet >= 0 ? inSet : 0);
  }

  if (outAdded == 0) {
    outputCombo_->addItem("<no writable v4l2loopback found>", "");
    outputCombo_->setEnabled(false);
  } else {
    outputCombo_->setEnabled(true);
    outputCombo_->setCurrentIndex(outSet >= 0 ? outSet : 0);
  }

  suggestedCmd_ = QString::fromStdString(rep.suggested_modprobe_cmd);
  copyCmdBtn_->setEnabled(!suggestedCmd_.isEmpty());

  UpdateStatusText();
  UpdateUiEnabled();
}

void VideoPage::CopySuggestedCommand() {
  if (suggestedCmd_.isEmpty()) return;
  if (auto* cb = QGuiApplication::clipboard()) cb->setText(suggestedCmd_);
}

void VideoPage::OnMirrorToggled(bool checked) {
  pipeline_.SetMirrorEnabled(checked);
}

void VideoPage::OnStart() {
  const auto st = pipeline_.Status();
  if (st.running || st.starting) return;

  const QString inDev = inputCombo_->currentData().toString();
  const QString outDev = outputCombo_->currentData().toString();

  if (inDev.isEmpty()) {
    ShowError("Start failed", "No input camera selected.");
    return;
  }
  if (outDev.isEmpty()) {
    ShowError("Start failed",
              "No v4l2loopback output found.\n\nLoad v4l2loopback (use the suggested modprobe command).");
    return;
  }

  studiocast::video::CameraPipelineConfig cfg;
  cfg.input_device = inDev.toStdString();
  cfg.output_device = outDev.toStdString();
  cfg.width = widthSpin_->value();
  cfg.height = heightSpin_->value();
  cfg.fps = fpsSpin_->value();
  cfg.effects.mirror = mirrorCheck_->isChecked();

  std::string err;
  if (!pipeline_.Start(cfg, &err)) {
    ShowError("Start failed", QString::fromStdString(err));
    Refresh();
    return;
  }

  UpdateStatusText();
  UpdateUiEnabled();
}

void VideoPage::OnStop() {
  pipeline_.Stop();
  UpdateStatusText();
  UpdateUiEnabled();
}

void VideoPage::OnPoll() {
  UpdateStatusText();
  UpdateUiEnabled();
}

void VideoPage::UpdateUiEnabled() {
  const auto st = pipeline_.Status();
  const bool busy = st.running || st.starting;

  refreshBtn_->setEnabled(!busy);
  copyCmdBtn_->setEnabled(!suggestedCmd_.isEmpty());

  inputCombo_->setEnabled(!busy && inputCombo_->count() > 0 && !inputCombo_->itemData(0).toString().isEmpty());
  outputCombo_->setEnabled(!busy && outputCombo_->count() > 0 && !outputCombo_->itemData(0).toString().isEmpty());

  widthSpin_->setEnabled(!busy);
  heightSpin_->setEnabled(!busy);
  fpsSpin_->setEnabled(!busy);

  // Mirror can be toggled while running (we update an atomic flag)
  mirrorCheck_->setEnabled(true);

  startBtn_->setEnabled(!busy && !inputCombo_->currentData().toString().isEmpty() &&
                        !outputCombo_->currentData().toString().isEmpty());
  stopBtn_->setEnabled(busy);
}

void VideoPage::UpdateStatusText() {
  const auto st = pipeline_.Status();

  std::ostringstream oss;
  oss << baseStatusText_ << "\n\n---\nPipeline\n";
  if (st.starting) oss << "  state: starting...\n";
  else if (st.running) oss << "  state: running\n";
  else oss << "  state: stopped\n";

  if (!st.input_device.empty()) oss << "  input:  " << st.input_device << "\n";
  if (!st.output_device.empty()) oss << "  output: " << st.output_device << "\n";

  if (st.running || st.starting) {
    oss << "  capture: " << st.capture.width << "x" << st.capture.height
        << " @ " << st.capture.fps << " fps (yuyv)\n";
    oss << "  output:  " << st.output.width << "x" << st.output.height
        << " @ " << st.output.fps << " fps (" << studiocast::video::PixelFormatName(st.output.format) << ")\n";
    oss << "  frames:  " << st.frame_index << "\n";
  }

  if (!st.last_error.empty()) {
    oss << "  last error: " << st.last_error << "\n";
  }

  oss << "\nNotes\n"
      << "  - Internal processing is RGB (CPU) for now; GPU/Maxine effects can slot in later.\n"
      << "  - v4l2loopback must be loaded before starting.\n";

  statusText_->setPlainText(QString::fromStdString(oss.str()));
}

}  // namespace studiocast::gui
