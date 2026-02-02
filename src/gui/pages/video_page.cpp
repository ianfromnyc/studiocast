#include "video_page.h"

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QGuiApplication>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QPixmap>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <sstream>

#include <algorithm>
#include <cstring>

#include "core/video/convert.h"
#include "core/video/v4l2loopback.h"

namespace studiocast::gui {
namespace {

QString DeviceLabel(const studiocast::video::VideoDevice& d) {
  QString label = QString::fromStdString(d.dev_node);
  if (!d.name.empty()) label += " — " + QString::fromStdString(d.name);
  if (!d.driver.empty()) label += " (" + QString::fromStdString(d.driver) + ")";
  if (d.is_loopback) label += " [loopback]";
  return label;
}

struct DaemonVideoStatus {
  bool reachable = false;

  bool enabled = false;
  bool always_on = false;

  bool consumer_present = false;
  int consumer_count = 0;

  bool pipeline_running = false;
  bool pipeline_starting = false;
  long long frame_index = 0;

  int width = 0;
  int height = 0;
  int fps = 0;
  bool mirror = false;

  QString input_device;
  QString output_device;
  QString last_error;
};

bool ParseDaemonStatusJson(const std::string& json, DaemonVideoStatus* out, QString* error) {
  if (!out) return false;

  QJsonParseError perr;
  const auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(json), &perr);
  if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
    if (error) *error = "JSON parse error: " + perr.errorString();
    return false;
  }

  const QJsonObject root = doc.object();
  const QJsonObject video = root.value("video").toObject();
  if (video.isEmpty()) {
    if (error) *error = "Missing 'video' object";
    return false;
  }

  out->reachable = true;
  out->enabled = video.value("enabled").toBool(false);
  out->always_on = video.value("always_on").toBool(false);
  out->consumer_present = video.value("consumer_present").toBool(false);
  out->consumer_count = video.value("consumer_count").toInt(0);

  out->width = video.value("width").toInt(0);
  out->height = video.value("height").toInt(0);
  out->fps = video.value("fps").toInt(0);
  out->mirror = video.value("mirror").toBool(false);

  out->input_device = video.value("input_device").toString();
  out->output_device = video.value("output_device").toString();

  const QJsonObject pipe = video.value("pipeline").toObject();
  out->pipeline_running = pipe.value("running").toBool(false);
  out->pipeline_starting = pipe.value("starting").toBool(false);
  out->frame_index = static_cast<long long>(pipe.value("frame_index").toDouble(0));

  out->last_error = video.value("last_error").toString();
  return true;
}

bool ParseDaemonConfigJson(const std::string& json,
                          bool* enabled,
                          QString* input,
                          QString* output,
                          int* width,
                          int* height,
                          int* fps,
                          bool* mirror,
                          QString* error) {
  QJsonParseError perr;
  const auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(json), &perr);
  if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
    if (error) *error = "JSON parse error: " + perr.errorString();
    return false;
  }

  const QJsonObject root = doc.object();
  if (enabled) *enabled = root.value("enabled").toBool(false);
  if (input) *input = root.value("input_device").toString();
  if (output) *output = root.value("output_device").toString();
  if (width) *width = root.value("width").toInt(0);
  if (height) *height = root.value("height").toInt(0);
  if (fps) *fps = root.value("fps").toInt(0);
  if (mirror) *mirror = root.value("mirror").toBool(false);
  return true;
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

}  // namespace

VideoPage::VideoPage(QWidget* parent) : QWidget(parent) {
  auto* root = new QVBoxLayout(this);
  root->setSpacing(12);

  auto* title = new QLabel("Camera", this);
  title->setStyleSheet("font-size: 20px; font-weight: 600;");
  root->addWidget(title);

  auto* box = new QGroupBox("Processed Camera → Virtual Camera (daemon-driven)", this);
  auto* boxLayout = new QVBoxLayout(box);

  // Preview
  previewLabel_ = new QLabel(box);
  previewLabel_->setMinimumHeight(240);
  previewLabel_->setAlignment(Qt::AlignCenter);
  previewLabel_->setStyleSheet("background: #111; border: 1px solid #333; color: #aaa;");
  previewLabel_->setText("Preview (opens the virtual camera as a consumer)");
  boxLayout->addWidget(previewLabel_);

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
  statusText_->setMinimumHeight(260);
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

  previewTimer_ = new QTimer(this);
  previewTimer_->setInterval(33);
  connect(previewTimer_, &QTimer::timeout, this, &VideoPage::OnPreviewTick);

  Refresh();
  SyncFromDaemonConfig();
  UpdateStatusText();
  UpdateUiEnabled();
}

VideoPage::~VideoPage() {
  StopPreview();
}

void VideoPage::ShowError(const QString& title, const QString& details) {
  QMessageBox::critical(this, title, details);
}

void VideoPage::Refresh() {
  const auto rep = studiocast::video::ProbeLoopback();
  baseStatusText_ = rep.ToText();

  const QString prevIn = inputCombo_->currentData().toString();
  const QString prevOut = outputCombo_->currentData().toString();

  inputCombo_->clear();
  outputCombo_->clear();

  // Always provide an explicit "auto" option so the daemon can choose.
  inputCombo_->addItem("<auto>", "auto");

  int inSet = (prevIn == "auto") ? 0 : -1;
  int outSet = (prevOut == "auto") ? 0 : -1;

  int inAdded = 0;
  int outAdded = 0;

  for (const auto& d : rep.devices) {
    const QString deviceNode = QString::fromStdString(d.dev_node);
    const QString label = DeviceLabel(d);

    if (!d.is_loopback && d.can_read) {
      inputCombo_->addItem(label, deviceNode);
      ++inAdded;
      if (!prevIn.isEmpty() && prevIn == deviceNode) inSet = inputCombo_->count() - 1;
    }

    if (d.is_loopback && d.can_write) {
      // Populate output combo lazily; insert <auto> only if we have at least one loopback.
      if (outputCombo_->count() == 0) {
        outputCombo_->addItem("<auto>", "auto");
      }
      outputCombo_->addItem(label, deviceNode);
      ++outAdded;
      if (!prevOut.isEmpty() && prevOut == deviceNode) outSet = outputCombo_->count() - 1;
    }
  }

  if (inAdded == 0) {
    inputCombo_->setEnabled(true);  // still allow <auto>
    inputCombo_->setCurrentIndex(0);
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

  SyncFromDaemonConfig();
  UpdateStatusText();
  UpdateUiEnabled();
}

void VideoPage::CopySuggestedCommand() {
  if (suggestedCmd_.isEmpty()) return;
  if (auto* cb = QGuiApplication::clipboard()) cb->setText(suggestedCmd_);
}

bool VideoPage::SyncFromDaemonConfig() {
  std::string json;
  QString err;
  if (!DaemonRequest("GET_CONFIG", &json, &err)) {
    daemonReachable_ = false;
    return false;
  }

  daemonReachable_ = true;

  bool enabled = false;
  QString input;
  QString output;
  int w = 0, h = 0, fps = 0;
  bool mirror = false;

  QString parseErr;
  if (!ParseDaemonConfigJson(json, &enabled, &input, &output, &w, &h, &fps, &mirror, &parseErr)) {
    return false;
  }

  // Apply to UI (best-effort; ignore if device not found in combo).
  const QString inKey = input.isEmpty() ? "auto" : input;
  const int inIdx = inputCombo_->findData(inKey);
  if (inIdx >= 0) inputCombo_->setCurrentIndex(inIdx);

  const QString outKey = output.isEmpty() ? "auto" : output;
  const int outIdx = outputCombo_->findData(outKey);
  if (outIdx >= 0) outputCombo_->setCurrentIndex(outIdx);

  if (w > 0) widthSpin_->setValue(w);
  if (h > 0) heightSpin_->setValue(h);
  if (fps > 0) fpsSpin_->setValue(fps);

  mirrorCheck_->blockSignals(true);
  mirrorCheck_->setChecked(mirror);
  mirrorCheck_->blockSignals(false);

  return true;
}

bool VideoPage::SendDaemonVideoConfig() {
  const QString inDev = inputCombo_->currentData().toString();
  const QString outDev = outputCombo_->currentData().toString();

  if (outDev.isEmpty()) {
    ShowError("Start failed",
              "No v4l2loopback output found.\n\nLoad v4l2loopback (use the suggested modprobe command).\n\n"
              "Note: studiocastd keeps the virtual device discoverable, but it cannot create the kernel module.");
    return false;
  }

  std::ostringstream req;
  req << "SET_VIDEO_CONFIG";
  req << " input=" << (inDev.isEmpty() ? "auto" : inDev.toStdString());
  req << " output=" << (outDev.isEmpty() ? "auto" : outDev.toStdString());
  req << " width=" << widthSpin_->value();
  req << " height=" << heightSpin_->value();
  req << " fps=" << fpsSpin_->value();

  QString err;
  if (!DaemonRequest(req.str(), nullptr, &err)) {
    ShowError("Start failed", "Failed to configure studiocastd:\n\n" + err +
                                 "\n\nIs studiocastd running?\nTry: ./build/studiocastd");
    return false;
  }

  return true;
}

bool VideoPage::SendDaemonMirror(bool enabled) {
  std::string req = std::string("SET_VIDEO_EFFECTS mirror=") + (enabled ? "1" : "0");
  QString err;
  if (!DaemonRequest(req, nullptr, &err)) {
    ShowError("Mirror update failed", err);
    return false;
  }
  return true;
}

bool VideoPage::SendDaemonEnabled(bool enabled) {
  std::string req = std::string("SET_ENABLED enabled=") + (enabled ? "1" : "0");
  QString err;
  if (!DaemonRequest(req, nullptr, &err)) {
    ShowError("Start/Stop failed", err);
    return false;
  }
  return true;
}

void VideoPage::OnMirrorToggled(bool checked) {
  (void)SendDaemonMirror(checked);
}

void VideoPage::OnStart() {
  if (!SendDaemonVideoConfig()) {
    UpdateStatusText();
    UpdateUiEnabled();
    return;
  }

  (void)SendDaemonMirror(mirrorCheck_->isChecked());
  (void)SendDaemonEnabled(true);

  StartPreview();
  UpdateStatusText();
  UpdateUiEnabled();
}

void VideoPage::OnStop() {
  StopPreview();
  (void)SendDaemonEnabled(false);
  UpdateStatusText();
  UpdateUiEnabled();
}

void VideoPage::OnPoll() {
  UpdateStatusText();
  UpdateUiEnabled();
}

void VideoPage::StartPreview() {
  StopPreview();

  // Determine which device to open for preview.
  QString outDev = outputCombo_->currentData().toString();
  if (outDev.isEmpty() || outDev == "auto") {
    // Ask daemon for the selected output in auto mode.
    std::string json;
    QString err;
    if (DaemonRequest("GET_STATUS", &json, &err)) {
      DaemonVideoStatus st;
      QString perr;
      if (ParseDaemonStatusJson(json, &st, &perr) && !st.output_device.isEmpty()) {
        outDev = st.output_device;
      }
    }
  }

  if (outDev.isEmpty() || outDev == "auto") {
    previewLabel_->setText("Preview unavailable (no output device selected)");
    return;
  }

  const int wantW = widthSpin_->value();
  const int wantH = heightSpin_->value();
  const int wantFps = fpsSpin_->value();

  std::string err;
  if (!previewCapture_.Open(outDev.toStdString(), wantW, wantH, wantFps, studiocast::video::CapturePixelFormat::rgb24, &err)) {
    std::string err2;
    if (!previewCapture_.Open(outDev.toStdString(), wantW, wantH, wantFps, studiocast::video::CapturePixelFormat::yuyv, &err2)) {
      previewLabel_->setText("Preview open failed:\n" + QString::fromStdString(err2));
      return;
    }
  }

  const auto fmt = previewCapture_.Actual();
  previewW_ = fmt.width;
  previewH_ = fmt.height;
  previewBpl_ = previewW_ * 3;
  previewRgb_.assign(static_cast<std::size_t>(previewBpl_ * previewH_), 0);

  previewTimer_->start();
  previewLabel_->setText("Preview starting...");
}

void VideoPage::StopPreview() {
  if (previewTimer_) previewTimer_->stop();
  if (previewCapture_.IsOpen()) previewCapture_.Close();
  previewRgb_.clear();
  previewW_ = previewH_ = previewBpl_ = 0;
}

void VideoPage::OnPreviewTick() {
  if (!previewCapture_.IsOpen() || previewRgb_.empty()) return;

  studiocast::video::CapturedFrameView f;
  std::string err;
  if (!previewCapture_.AcquireFrame(&f, 0, &err)) {
    // Normal: no frame ready yet.
    return;
  }

  const auto fmt = previewCapture_.Actual();

  if (fmt.format == studiocast::video::CapturePixelFormat::rgb24) {
    const std::size_t srcStride = fmt.bytes_per_line;
    const std::size_t dstStride = static_cast<std::size_t>(previewBpl_);
    const auto* src = f.data;
    auto* dst = previewRgb_.data();
    for (int y = 0; y < previewH_; ++y) {
      std::memcpy(dst + static_cast<std::size_t>(y) * dstStride,
                  src + static_cast<std::size_t>(y) * srcStride,
                  std::min(dstStride, srcStride));
    }
  } else {
    studiocast::video::YuyvToRgb24(f.data,
                                  previewW_,
                                  previewH_,
                                  fmt.bytes_per_line,
                                  previewRgb_.data(),
                                  static_cast<std::size_t>(previewBpl_));
  }

  std::string rerr;
  (void)previewCapture_.ReleaseFrame(f, &rerr);

  QImage img(previewRgb_.data(), previewW_, previewH_, previewBpl_, QImage::Format_RGB888);
  previewLabel_->setPixmap(QPixmap::fromImage(img).scaled(previewLabel_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void VideoPage::UpdateUiEnabled() {
  // Query daemon status (best-effort) to determine whether controls should be editable.
  DaemonVideoStatus st;
  if (daemonReachable_ && !daemonLastStatusJson_.empty()) {
    QString perr;
    (void)ParseDaemonStatusJson(daemonLastStatusJson_, &st, &perr);
  }

  const bool enabled = daemonReachable_ ? st.enabled : false;

  refreshBtn_->setEnabled(!enabled);
  copyCmdBtn_->setEnabled(!suggestedCmd_.isEmpty());

  const bool outSelectable = outputCombo_->count() > 0 && !outputCombo_->itemData(0).toString().isEmpty();

  inputCombo_->setEnabled(!enabled);
  outputCombo_->setEnabled(!enabled && outSelectable);

  widthSpin_->setEnabled(!enabled);
  heightSpin_->setEnabled(!enabled);
  fpsSpin_->setEnabled(!enabled);

  // Mirror can be toggled while running.
  mirrorCheck_->setEnabled(daemonReachable_);

  startBtn_->setEnabled(daemonReachable_ && !enabled && outSelectable && !outputCombo_->currentData().toString().isEmpty());
  stopBtn_->setEnabled(daemonReachable_ && enabled);

  // Keep preview in sync with enabled state.
  if (enabled && daemonReachable_ && !previewCapture_.IsOpen()) {
    StartPreview();
  }
  if (!enabled && previewCapture_.IsOpen()) {
    StopPreview();
  }
}

void VideoPage::UpdateStatusText() {
  DaemonVideoStatus st;
  daemonReachable_ = false;
  daemonLastStatusJson_.clear();

  QString derr;
  std::string json;
  if (DaemonRequest("GET_STATUS", &json, &derr)) {
    daemonReachable_ = true;
    daemonLastStatusJson_ = json;
    QString perr;
    (void)ParseDaemonStatusJson(json, &st, &perr);
  }

  std::ostringstream oss;
  oss << baseStatusText_ << "\n\n---\nDaemon (studiocastd)\n";

  if (!daemonReachable_) {
    oss << "  status: not running / not reachable\n";
    oss << "\nTips\n"
        << "  - Start the daemon in a terminal:\n"
        << "      ./build/studiocastd\n"
        << "  - Or enable the systemd user service (packaging step).\n";

    statusText_->setPlainText(QString::fromStdString(oss.str()));
    return;
  }

  oss << "  enabled:    " << (st.enabled ? "yes" : "no") << "\n";
  oss << "  consumers:  " << st.consumer_count << (st.consumer_present ? " (present)" : "") << "\n";
  oss << "  pipeline:   " << (st.pipeline_running ? "running" : (st.pipeline_starting ? "starting" : "stopped"))
      << "\n";
  oss << "  input:      " << st.input_device.toStdString() << "\n";
  oss << "  output:     " << st.output_device.toStdString() << "\n";
  oss << "  requested:  " << st.width << "x" << st.height << " @ " << st.fps << " fps\n";
  oss << "  mirror:     " << (st.mirror ? "on" : "off") << "\n";
  oss << "  frames:     " << st.frame_index << "\n";

  if (!st.last_error.isEmpty()) {
    oss << "  last error: " << st.last_error.toStdString() << "\n";
  }

  oss << "\nNotes\n"
      << "  - The daemon only runs heavy processing when consumers are present (OBS/Zoom/GUI preview).\n"
      << "  - Preview counts as a consumer: it opens the v4l2loopback device for capture.\n";

  statusText_->setPlainText(QString::fromStdString(oss.str()));
}

}  // namespace studiocast::gui
