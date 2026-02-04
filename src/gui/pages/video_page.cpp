#include "video_page.h"

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QFileInfo>
#include <QGuiApplication>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
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

  QString background;
  QString background_backend;
  int background_strength = 0;
  QString background_remove_color;
  QString background_replace_image;

  // Virtual Key Light (Video Relighting)
  bool virtual_key_light = false;
  int virtual_key_light_intensity = 0;  // 0..100
  QString virtual_key_light_temperature;
  int virtual_key_light_pan = 0;
  QString virtual_key_light_hdri;

  // Maxine runtime diagnostics (from daemon GET_STATUS)
  bool maxine_ok = false;
  bool maxine_supported = false;
  QString maxine_summary;
  QString maxine_blocked_reason;
  QStringList maxine_blocked_details;
  QStringList maxine_available_effects;
  bool virtual_key_light_available = false;

  QString effects_backends;
  QString effects_note;

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

  out->background = video.value("background").toString();
  out->background_backend = video.value("background_backend").toString();
  out->background_strength = video.value("background_strength").toInt(0);
  out->background_remove_color = video.value("background_remove_color").toString();
  out->background_replace_image = video.value("background_replace_image").toString();

  out->virtual_key_light = video.value("virtual_key_light").toBool(false);
  out->virtual_key_light_intensity = video.value("virtual_key_light_intensity").toInt(0);
  out->virtual_key_light_temperature = video.value("virtual_key_light_temperature").toString();
  out->virtual_key_light_pan = video.value("virtual_key_light_pan").toInt(0);
  out->virtual_key_light_hdri = video.value("virtual_key_light_hdri").toString();

  out->input_device = video.value("input_device").toString();
  out->output_device = video.value("output_device").toString();

  const QJsonObject pipe = video.value("pipeline").toObject();
  out->pipeline_running = pipe.value("running").toBool(false);
  out->pipeline_starting = pipe.value("starting").toBool(false);
  out->frame_index = static_cast<long long>(pipe.value("frame_index").toDouble(0));

  out->effects_backends = pipe.value("effects_backends").toString();
  out->effects_note = pipe.value("effects_note").toString();

  const QJsonObject maxine = root.value("maxine").toObject();
  if (!maxine.isEmpty()) {
    out->maxine_ok = maxine.value("ok").toBool(false);
    out->maxine_supported = maxine.value("supported").toBool(out->maxine_ok);
    out->maxine_summary = maxine.value("summary").toString();
    out->maxine_blocked_reason = maxine.value("blocked_reason").toString();

    out->maxine_blocked_details.clear();
    const auto blocked = maxine.value("blocked_details").toArray();
    for (const auto& v : blocked) {
      const QString s = v.toString();
      if (!s.isEmpty()) out->maxine_blocked_details.push_back(s);
    }

    out->maxine_available_effects.clear();
    out->virtual_key_light_available = false;
    const auto arr = maxine.value("available_effects").toArray();
    for (const auto& v : arr) {
      const QString id = v.toString();
      if (!id.isEmpty()) out->maxine_available_effects.push_back(id);
      if (id == "virtual_key_light") {
        out->virtual_key_light_available = true;
      }
    }
  }

  out->last_error = video.value("last_error").toString();
  return true;
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

  // Effect engine (Maxine-only UX)
  auto* engineRow = new QHBoxLayout();
  engineRow->addWidget(new QLabel("Effect engine:", box));
  effectEngineValue_ = new QLabel("Maxine", box);
  effectEngineValue_->setStyleSheet("font-weight: 600;");
  engineRow->addWidget(effectEngineValue_);
  engineRow->addStretch(1);
  boxLayout->addLayout(engineRow);

  maxineBanner_ = new QLabel(box);
  maxineBanner_->setWordWrap(true);
  maxineBanner_->setStyleSheet(
      "background: #3a1414; border: 1px solid #663333; color: #f0d0d0; padding: 8px; border-radius: 4px;");
  maxineBanner_->setVisible(false);
  boxLayout->addWidget(maxineBanner_);

  // Mirror
  auto* fxRow = new QHBoxLayout();
  mirrorCheck_ = new QCheckBox("Mirror (horizontal flip)", box);
  fxRow->addWidget(mirrorCheck_);
  fxRow->addStretch(1);
  boxLayout->addLayout(fxRow);

  // Virtual Background
  auto* vbBox = new QGroupBox("Virtual Background", box);
  auto* vbLayout = new QVBoxLayout(vbBox);

  auto* vbRow = new QHBoxLayout();
  vbRow->addWidget(new QLabel("Mode:", vbBox));
  backgroundCombo_ = new QComboBox(vbBox);
  backgroundCombo_->addItem("None", "none");
  backgroundCombo_->addItem("Blur", "blur");
  backgroundCombo_->addItem("Remove", "remove");
  backgroundCombo_->addItem("Replace", "replace");
  vbRow->addWidget(backgroundCombo_, 1);
  vbRow->addSpacing(12);
  vbRow->addWidget(new QLabel("Blur strength:", vbBox));
  backgroundStrengthSpin_ = new QSpinBox(vbBox);
  backgroundStrengthSpin_->setRange(0, 100);
  backgroundStrengthSpin_->setValue(50);
  backgroundStrengthSpin_->setSuffix("%");
  backgroundStrengthSpin_->setMaximumWidth(90);
  vbRow->addWidget(backgroundStrengthSpin_);
  vbLayout->addLayout(vbRow);

  auto* vbParamRow = new QHBoxLayout();
  vbParamRow->addWidget(new QLabel("Remove color (#RRGGBB):", vbBox));
  backgroundRemoveColorEdit_ = new QLineEdit(vbBox);
  backgroundRemoveColorEdit_->setPlaceholderText("#000000");
  backgroundRemoveColorEdit_->setMaximumWidth(110);
  vbParamRow->addWidget(backgroundRemoveColorEdit_);

  vbParamRow->addSpacing(12);
  vbParamRow->addWidget(new QLabel("Replace image:", vbBox));
  backgroundReplaceImageEdit_ = new QLineEdit(vbBox);
  vbParamRow->addWidget(backgroundReplaceImageEdit_, 1);
  browseReplaceImageBtn_ = new QPushButton("Browse…", vbBox);
  vbParamRow->addWidget(browseReplaceImageBtn_);
  vbLayout->addLayout(vbParamRow);

  boxLayout->addWidget(vbBox);

  // Auto Frame
  auto* afBox = new QGroupBox("Auto Frame", box);
  auto* afLayout = new QHBoxLayout(afBox);
  autoFrameCheck_ = new QCheckBox("Enable", afBox);
  afLayout->addWidget(autoFrameCheck_);
  afLayout->addSpacing(12);
  afLayout->addWidget(new QLabel("Zoom:", afBox));
  autoFrameZoomSlider_ = new QSlider(Qt::Horizontal, afBox);
  autoFrameZoomSlider_->setRange(0, 100);
  autoFrameZoomSlider_->setValue(50);
  afLayout->addWidget(autoFrameZoomSlider_, 1);
  autoFrameZoomValue_ = new QLabel("50%", afBox);
  autoFrameZoomValue_->setMinimumWidth(44);
  autoFrameZoomValue_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  afLayout->addWidget(autoFrameZoomValue_);
  boxLayout->addWidget(afBox);

  // Eye Contact
  auto* ecBox = new QGroupBox("Eye Contact", box);
  auto* ecLayout = new QHBoxLayout(ecBox);
  eyeContactCheck_ = new QCheckBox("Enable", ecBox);
  ecLayout->addWidget(eyeContactCheck_);
  ecLayout->addSpacing(12);
  ecLayout->addWidget(new QLabel("Strength:", ecBox));
  eyeContactStrengthSlider_ = new QSlider(Qt::Horizontal, ecBox);
  eyeContactStrengthSlider_->setRange(0, 100);
  eyeContactStrengthSlider_->setValue(50);
  ecLayout->addWidget(eyeContactStrengthSlider_, 1);
  eyeContactStrengthValue_ = new QLabel("50%", ecBox);
  eyeContactStrengthValue_->setMinimumWidth(44);
  eyeContactStrengthValue_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  ecLayout->addWidget(eyeContactStrengthValue_);
  ecLayout->addSpacing(12);
  eyeContactLookAwayCheck_ = new QCheckBox("Allow look-away", ecBox);
  eyeContactLookAwayCheck_->setChecked(true);
  ecLayout->addWidget(eyeContactLookAwayCheck_);
  boxLayout->addWidget(ecBox);

  // Video Noise Removal
  auto* dnBox = new QGroupBox("Video Noise Removal", box);
  auto* dnLayout = new QHBoxLayout(dnBox);
  denoiseCheck_ = new QCheckBox("Enable", dnBox);
  dnLayout->addWidget(denoiseCheck_);
  dnLayout->addSpacing(12);
  dnLayout->addWidget(new QLabel("Strength:", dnBox));
  denoiseStrengthSlider_ = new QSlider(Qt::Horizontal, dnBox);
  denoiseStrengthSlider_->setRange(0, 100);
  denoiseStrengthSlider_->setValue(50);
  dnLayout->addWidget(denoiseStrengthSlider_, 1);
  denoiseStrengthValue_ = new QLabel("50%", dnBox);
  denoiseStrengthValue_->setMinimumWidth(44);
  denoiseStrengthValue_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  dnLayout->addWidget(denoiseStrengthValue_);
  boxLayout->addWidget(dnBox);

  // Virtual Key Light
  auto* vklBox = new QGroupBox("Virtual Key Light", box);
  auto* vklLayout = new QVBoxLayout(vklBox);

  auto* vklRow = new QHBoxLayout();
  virtualKeyLightCheck_ = new QCheckBox("Enable", vklBox);
  vklRow->addWidget(virtualKeyLightCheck_);
  vklRow->addSpacing(12);

  vklRow->addWidget(new QLabel("Intensity:", vklBox));
  virtualKeyLightIntensitySpin_ = new QSpinBox(vklBox);
  virtualKeyLightIntensitySpin_->setRange(0, 100);
  virtualKeyLightIntensitySpin_->setValue(70);
  virtualKeyLightIntensitySpin_->setSuffix("%");
  virtualKeyLightIntensitySpin_->setMaximumWidth(90);
  vklRow->addWidget(virtualKeyLightIntensitySpin_);

  vklRow->addSpacing(12);
  vklRow->addWidget(new QLabel("Temp:", vklBox));
  virtualKeyLightTempCombo_ = new QComboBox(vklBox);
  virtualKeyLightTempCombo_->addItem("Neutral", "neutral");
  virtualKeyLightTempCombo_->addItem("Warm", "warm");
  virtualKeyLightTempCombo_->addItem("Cool", "cool");
  vklRow->addWidget(virtualKeyLightTempCombo_);

  vklRow->addSpacing(12);
  vklRow->addWidget(new QLabel("Pan:", vklBox));
  virtualKeyLightPanSpin_ = new QSpinBox(vklBox);
  virtualKeyLightPanSpin_->setRange(-180, 180);
  virtualKeyLightPanSpin_->setValue(0);
  virtualKeyLightPanSpin_->setSuffix("°");
  virtualKeyLightPanSpin_->setMaximumWidth(90);
  vklRow->addWidget(virtualKeyLightPanSpin_);

  vklRow->addStretch(1);
  vklLayout->addLayout(vklRow);

  auto* vklRow2 = new QHBoxLayout();
  vklRow2->addWidget(new QLabel("HDRI (optional):", vklBox));
  virtualKeyLightHdriEdit_ = new QLineEdit(vklBox);
  vklRow2->addWidget(virtualKeyLightHdriEdit_, 1);
  browseVirtualKeyLightHdriBtn_ = new QPushButton("Browse…", vklBox);
  vklRow2->addWidget(browseVirtualKeyLightHdriBtn_);
  vklLayout->addLayout(vklRow2);

  boxLayout->addWidget(vklBox);

  // Vignette
  auto* vigBox = new QGroupBox("Vignette", box);
  auto* vigLayout = new QHBoxLayout(vigBox);
  vignetteCheck_ = new QCheckBox("Enable", vigBox);
  vigLayout->addWidget(vignetteCheck_);
  vigLayout->addSpacing(12);
  vigLayout->addWidget(new QLabel("Intensity:", vigBox));
  vignetteIntensitySlider_ = new QSlider(Qt::Horizontal, vigBox);
  vignetteIntensitySlider_->setRange(0, 100);
  vignetteIntensitySlider_->setValue(35);
  vigLayout->addWidget(vignetteIntensitySlider_, 1);
  vignetteIntensityValue_ = new QLabel("35%", vigBox);
  vignetteIntensityValue_->setMinimumWidth(44);
  vignetteIntensityValue_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  vigLayout->addWidget(vignetteIntensityValue_);
  vigLayout->addSpacing(12);
  vignetteCenterOnFaceCheck_ = new QCheckBox("Center on Auto Frame subject", vigBox);
  vignetteCenterOnFaceCheck_->setChecked(true);
  vigLayout->addWidget(vignetteCenterOnFaceCheck_);
  boxLayout->addWidget(vigBox);

  // Diagnostics expander
  auto* diagBox = new QGroupBox("Diagnostics", box);
  diagBox->setCheckable(true);
  diagBox->setChecked(false);
  auto* diagLayout = new QVBoxLayout(diagBox);
  openInstallHintsBtn_ = new QPushButton("Open install hints", diagBox);
  openInstallHintsBtn_->setVisible(false);
  diagLayout->addWidget(openInstallHintsBtn_, 0, Qt::AlignLeft);
  diagnosticsText_ = new QPlainTextEdit(diagBox);
  diagnosticsText_->setReadOnly(true);
  diagnosticsText_->setMinimumHeight(120);
  diagnosticsText_->setVisible(false);
  diagLayout->addWidget(diagnosticsText_, 1);
  connect(diagBox, &QGroupBox::toggled, this, [this](bool on) {
    if (openInstallHintsBtn_) openInstallHintsBtn_->setVisible(on);
    if (diagnosticsText_) diagnosticsText_->setVisible(on);
  });
  boxLayout->addWidget(diagBox);

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
  connect(backgroundCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &VideoPage::OnBackgroundChanged);
  connect(backgroundStrengthSpin_, QOverload<int>::of(&QSpinBox::valueChanged),
          this, &VideoPage::OnBackgroundStrengthChanged);

  connect(backgroundRemoveColorEdit_, &QLineEdit::editingFinished, this, &VideoPage::OnBackgroundRemoveColorChanged);
  connect(backgroundReplaceImageEdit_, &QLineEdit::editingFinished, this, &VideoPage::OnBackgroundReplaceImageChanged);
  connect(browseReplaceImageBtn_, &QPushButton::clicked, this, &VideoPage::OnBrowseReplaceImage);

  connect(autoFrameCheck_, &QCheckBox::toggled, this, &VideoPage::OnAutoFrameToggled);
  connect(autoFrameZoomSlider_, &QSlider::valueChanged, this, &VideoPage::OnAutoFrameZoomChanged);

  connect(eyeContactCheck_, &QCheckBox::toggled, this, &VideoPage::OnEyeContactToggled);
  connect(eyeContactStrengthSlider_, &QSlider::valueChanged, this, &VideoPage::OnEyeContactStrengthChanged);
  connect(eyeContactLookAwayCheck_, &QCheckBox::toggled, this, &VideoPage::OnEyeContactLookAwayToggled);

  connect(denoiseCheck_, &QCheckBox::toggled, this, &VideoPage::OnDenoiseToggled);
  connect(denoiseStrengthSlider_, &QSlider::valueChanged, this, &VideoPage::OnDenoiseStrengthChanged);

  connect(openInstallHintsBtn_, &QPushButton::clicked, this, &VideoPage::OnOpenInstallHints);

  connect(virtualKeyLightCheck_, &QCheckBox::toggled, this, &VideoPage::OnVirtualKeyLightToggled);
  connect(virtualKeyLightIntensitySpin_, QOverload<int>::of(&QSpinBox::valueChanged),
          this, &VideoPage::OnVirtualKeyLightIntensityChanged);
  connect(virtualKeyLightTempCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &VideoPage::OnVirtualKeyLightTemperatureChanged);
  connect(virtualKeyLightPanSpin_, QOverload<int>::of(&QSpinBox::valueChanged),
          this, &VideoPage::OnVirtualKeyLightPanChanged);
  connect(virtualKeyLightHdriEdit_, &QLineEdit::editingFinished, this, &VideoPage::OnVirtualKeyLightHdriChanged);
  connect(browseVirtualKeyLightHdriBtn_, &QPushButton::clicked, this, &VideoPage::OnBrowseVirtualKeyLightHdri);

  connect(vignetteCheck_, &QCheckBox::toggled, this, &VideoPage::OnVignetteToggled);
  connect(vignetteIntensitySlider_, &QSlider::valueChanged, this, &VideoPage::OnVignetteIntensityChanged);
  connect(vignetteCenterOnFaceCheck_, &QCheckBox::toggled, this, &VideoPage::OnVignetteCenterOnFaceToggled);

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

  QJsonObject root;
  QString parseErr;
  if (!ParseJsonObject(json, &root, &parseErr)) {
    return false;
  }

  const QString input = root.value("input_device").toString();
  const QString output = root.value("output_device").toString();
  const int w = root.value("width").toInt(0);
  const int h = root.value("height").toInt(0);
  const int fps = root.value("fps").toInt(0);

  const QJsonObject fx = root.value("video_effects").toObject();

  const bool mirror = fx.isEmpty() ? root.value("mirror").toBool(false) : fx.value("mirror").toBool(false);

  // Virtual Background
  QString vbMode = "none";
  int vbBlurStrength = 50;
  QString vbRemoveColor;
  QString vbReplacePath;
  const QJsonObject vb = fx.value("virtual_background").toObject();
  if (!vb.isEmpty()) {
    vbMode = vb.value("mode").toString("none");
    vbBlurStrength = vb.value("blur_strength").toInt(50);
    vbRemoveColor = vb.value("remove_color").toString();
    vbReplacePath = vb.value("replace_path").toString();
  } else {
    vbMode = root.value("background").toString("none");
    vbBlurStrength = root.value("background_strength").toInt(50);
    vbRemoveColor = root.value("background_remove_color").toString();
    vbReplacePath = root.value("background_replace_image").toString();
  }

  // Auto Frame
  bool autoFrame = false;
  int autoFrameZoom = 50;
  const QJsonObject af = fx.value("auto_frame").toObject();
  if (!af.isEmpty()) {
    autoFrame = af.value("enabled").toBool(false);
    autoFrameZoom = af.value("zoom").toInt(50);
  }

  // Eye Contact
  bool eyeContact = false;
  int eyeContactStrength = 50;
  bool eyeContactLookAway = true;
  const QJsonObject ec = fx.value("eye_contact").toObject();
  if (!ec.isEmpty()) {
    eyeContact = ec.value("enabled").toBool(false);
    eyeContactStrength = ec.value("strength").toInt(50);
    eyeContactLookAway = ec.value("look_away").toBool(true);
  }

  // Video Noise Removal
  bool denoise = false;
  int denoiseStrength = 50;
  const QJsonObject dn = fx.value("video_noise_removal").toObject();
  if (!dn.isEmpty()) {
    denoise = dn.value("enabled").toBool(false);
    denoiseStrength = dn.value("strength").toInt(50);
  }

  // Virtual Key Light
  bool virtualKeyLight = false;
  int virtualKeyLightIntensity = 70;
  QString virtualKeyLightTemp = "neutral";
  int virtualKeyLightPan = 0;
  QString virtualKeyLightHdri;
  const QJsonObject vkl = fx.value("virtual_key_light").toObject();
  if (!vkl.isEmpty()) {
    virtualKeyLight = vkl.value("enabled").toBool(false);
    virtualKeyLightIntensity = vkl.value("intensity").toInt(70);
    virtualKeyLightTemp = vkl.value("temperature").toString("neutral");
    virtualKeyLightPan = vkl.value("pan").toInt(0);
    virtualKeyLightHdri = vkl.value("hdri_path").toString();
  } else {
    virtualKeyLight = root.value("virtual_key_light").toBool(false);
    virtualKeyLightIntensity = root.value("virtual_key_light_intensity").toInt(70);
    virtualKeyLightTemp = root.value("virtual_key_light_temperature").toString("neutral");
    virtualKeyLightPan = root.value("virtual_key_light_pan").toInt(0);
    virtualKeyLightHdri = root.value("virtual_key_light_hdri").toString();
  }

  // Vignette
  bool vignette = false;
  int vignetteIntensity = 35;
  bool vignetteCenterOnFace = true;
  const QJsonObject vg = fx.value("vignette").toObject();
  if (!vg.isEmpty()) {
    vignette = vg.value("enabled").toBool(false);
    vignetteIntensity = vg.value("intensity").toInt(35);
    vignetteCenterOnFace = vg.value("center_on_face").toBool(true);
  } else {
    vignette = root.value("vignette").toBool(false);
    vignetteIntensity = root.value("vignette_intensity").toInt(35);
    vignetteCenterOnFace = root.value("vignette_center_on_face").toBool(true);
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

  if (backgroundCombo_) {
    backgroundCombo_->blockSignals(true);
    const QString key = vbMode.isEmpty() ? "none" : vbMode;
    const int idx = backgroundCombo_->findData(key);
    if (idx >= 0) backgroundCombo_->setCurrentIndex(idx);
    backgroundCombo_->blockSignals(false);
  }

  if (backgroundStrengthSpin_) {
    backgroundStrengthSpin_->blockSignals(true);
    backgroundStrengthSpin_->setValue(std::max(0, std::min(100, vbBlurStrength)));
    backgroundStrengthSpin_->blockSignals(false);
  }

  if (backgroundRemoveColorEdit_) {
    backgroundRemoveColorEdit_->blockSignals(true);
    backgroundRemoveColorEdit_->setText(vbRemoveColor);
    backgroundRemoveColorEdit_->blockSignals(false);
  }
  if (backgroundReplaceImageEdit_) {
    backgroundReplaceImageEdit_->blockSignals(true);
    backgroundReplaceImageEdit_->setText(vbReplacePath);
    backgroundReplaceImageEdit_->blockSignals(false);
  }

  if (autoFrameCheck_) {
    autoFrameCheck_->blockSignals(true);
    autoFrameCheck_->setChecked(autoFrame);
    autoFrameCheck_->blockSignals(false);
  }
  if (autoFrameZoomSlider_) {
    autoFrameZoomSlider_->blockSignals(true);
    autoFrameZoomSlider_->setValue(std::max(0, std::min(100, autoFrameZoom)));
    autoFrameZoomSlider_->blockSignals(false);
  }
  if (autoFrameZoomValue_ && autoFrameZoomSlider_) {
    autoFrameZoomValue_->setText(QString::number(autoFrameZoomSlider_->value()) + "%");
  }

  if (eyeContactCheck_) {
    eyeContactCheck_->blockSignals(true);
    eyeContactCheck_->setChecked(eyeContact);
    eyeContactCheck_->blockSignals(false);
  }
  if (eyeContactStrengthSlider_) {
    eyeContactStrengthSlider_->blockSignals(true);
    eyeContactStrengthSlider_->setValue(std::max(0, std::min(100, eyeContactStrength)));
    eyeContactStrengthSlider_->blockSignals(false);
  }
  if (eyeContactStrengthValue_ && eyeContactStrengthSlider_) {
    eyeContactStrengthValue_->setText(QString::number(eyeContactStrengthSlider_->value()) + "%");
  }
  if (eyeContactLookAwayCheck_) {
    eyeContactLookAwayCheck_->blockSignals(true);
    eyeContactLookAwayCheck_->setChecked(eyeContactLookAway);
    eyeContactLookAwayCheck_->blockSignals(false);
  }

  if (denoiseCheck_) {
    denoiseCheck_->blockSignals(true);
    denoiseCheck_->setChecked(denoise);
    denoiseCheck_->blockSignals(false);
  }
  if (denoiseStrengthSlider_) {
    denoiseStrengthSlider_->blockSignals(true);
    denoiseStrengthSlider_->setValue(std::max(0, std::min(100, denoiseStrength)));
    denoiseStrengthSlider_->blockSignals(false);
  }
  if (denoiseStrengthValue_ && denoiseStrengthSlider_) {
    denoiseStrengthValue_->setText(QString::number(denoiseStrengthSlider_->value()) + "%");
  }

  if (virtualKeyLightCheck_) {
    virtualKeyLightCheck_->blockSignals(true);
    virtualKeyLightCheck_->setChecked(virtualKeyLight);
    virtualKeyLightCheck_->blockSignals(false);
  }
  if (virtualKeyLightIntensitySpin_) {
    virtualKeyLightIntensitySpin_->blockSignals(true);
    virtualKeyLightIntensitySpin_->setValue(std::max(0, std::min(100, virtualKeyLightIntensity)));
    virtualKeyLightIntensitySpin_->blockSignals(false);
  }
  if (virtualKeyLightTempCombo_) {
    virtualKeyLightTempCombo_->blockSignals(true);
    const QString key = virtualKeyLightTemp.isEmpty() ? "neutral" : virtualKeyLightTemp;
    const int idx = virtualKeyLightTempCombo_->findData(key);
    if (idx >= 0) virtualKeyLightTempCombo_->setCurrentIndex(idx);
    virtualKeyLightTempCombo_->blockSignals(false);
  }
  if (virtualKeyLightPanSpin_) {
    virtualKeyLightPanSpin_->blockSignals(true);
    virtualKeyLightPanSpin_->setValue(std::max(-180, std::min(180, virtualKeyLightPan)));
    virtualKeyLightPanSpin_->blockSignals(false);
  }
  if (virtualKeyLightHdriEdit_) {
    virtualKeyLightHdriEdit_->blockSignals(true);
    virtualKeyLightHdriEdit_->setText(virtualKeyLightHdri);
    virtualKeyLightHdriEdit_->blockSignals(false);
  }

  if (vignetteCheck_) {
    vignetteCheck_->blockSignals(true);
    vignetteCheck_->setChecked(vignette);
    vignetteCheck_->blockSignals(false);
  }
  if (vignetteIntensitySlider_) {
    vignetteIntensitySlider_->blockSignals(true);
    vignetteIntensitySlider_->setValue(std::max(0, std::min(100, vignetteIntensity)));
    vignetteIntensitySlider_->blockSignals(false);
  }
  if (vignetteIntensityValue_ && vignetteIntensitySlider_) {
    vignetteIntensityValue_->setText(QString::number(vignetteIntensitySlider_->value()) + "%");
  }
  if (vignetteCenterOnFaceCheck_) {
    vignetteCenterOnFaceCheck_->blockSignals(true);
    vignetteCenterOnFaceCheck_->setChecked(vignetteCenterOnFace);
    vignetteCenterOnFaceCheck_->blockSignals(false);
  }

  // Enable per-effect parameter controls.
  const QString bgMode = backgroundCombo_ ? backgroundCombo_->currentData().toString() : QString();
  if (backgroundStrengthSpin_) backgroundStrengthSpin_->setEnabled(bgMode == "blur");
  if (backgroundRemoveColorEdit_) backgroundRemoveColorEdit_->setEnabled(bgMode == "remove");
  if (backgroundReplaceImageEdit_) backgroundReplaceImageEdit_->setEnabled(bgMode == "replace");
  if (browseReplaceImageBtn_) browseReplaceImageBtn_->setEnabled(bgMode == "replace");

  if (autoFrameZoomSlider_ && autoFrameCheck_) autoFrameZoomSlider_->setEnabled(autoFrameCheck_->isChecked());
  if (eyeContactStrengthSlider_ && eyeContactCheck_) eyeContactStrengthSlider_->setEnabled(eyeContactCheck_->isChecked());
  if (eyeContactLookAwayCheck_ && eyeContactCheck_) eyeContactLookAwayCheck_->setEnabled(eyeContactCheck_->isChecked());
  if (denoiseStrengthSlider_ && denoiseCheck_) denoiseStrengthSlider_->setEnabled(denoiseCheck_->isChecked());

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

bool VideoPage::SendDaemonVideoEffects() {
  QJsonObject effects;
  effects.insert("mirror", mirrorCheck_ && mirrorCheck_->isChecked());

  // Product rule: Maxine is the only production effect engine.
  effects.insert("engine", "maxine");

  const QString bg = backgroundCombo_ ? backgroundCombo_->currentData().toString() : QString();
  {
    QJsonObject vb;
    if (!bg.isEmpty()) vb.insert("mode", bg);
    if (backgroundStrengthSpin_) vb.insert("blur_strength", backgroundStrengthSpin_->value());

    if (backgroundRemoveColorEdit_) {
      const QString c = backgroundRemoveColorEdit_->text().trimmed();
      if (!c.isEmpty()) vb.insert("remove_color", c);
    }
    if (backgroundReplaceImageEdit_) {
      const QString p = backgroundReplaceImageEdit_->text().trimmed();
      if (!p.isEmpty()) vb.insert("replace_path", p);
    }

    if (!vb.isEmpty()) effects.insert("virtual_background", vb);
  }

  // Auto Frame
  if (autoFrameCheck_ || autoFrameZoomSlider_) {
    QJsonObject af;
    if (autoFrameCheck_) af.insert("enabled", autoFrameCheck_->isChecked());
    if (autoFrameZoomSlider_) af.insert("zoom", autoFrameZoomSlider_->value());
    if (!af.isEmpty()) effects.insert("auto_frame", af);
  }

  // Eye Contact
  if (eyeContactCheck_ || eyeContactStrengthSlider_ || eyeContactLookAwayCheck_) {
    QJsonObject ec;
    if (eyeContactCheck_) ec.insert("enabled", eyeContactCheck_->isChecked());
    if (eyeContactStrengthSlider_) ec.insert("strength", eyeContactStrengthSlider_->value());
    if (eyeContactLookAwayCheck_) ec.insert("look_away", eyeContactLookAwayCheck_->isChecked());
    if (!ec.isEmpty()) effects.insert("eye_contact", ec);
  }

  // Video Noise Removal
  if (denoiseCheck_ || denoiseStrengthSlider_) {
    QJsonObject dn;
    if (denoiseCheck_) dn.insert("enabled", denoiseCheck_->isChecked());
    if (denoiseStrengthSlider_) dn.insert("strength", denoiseStrengthSlider_->value());
    if (!dn.isEmpty()) effects.insert("video_noise_removal", dn);
  }

  // Virtual Key Light (Maxine relighting)
  if (virtualKeyLightCheck_ || virtualKeyLightIntensitySpin_ || virtualKeyLightTempCombo_ ||
      virtualKeyLightPanSpin_ || virtualKeyLightHdriEdit_) {
    QJsonObject vkl;
    if (virtualKeyLightCheck_) vkl.insert("enabled", virtualKeyLightCheck_->isChecked());
    if (virtualKeyLightIntensitySpin_) vkl.insert("intensity", virtualKeyLightIntensitySpin_->value());
    if (virtualKeyLightTempCombo_) {
      const QString t = virtualKeyLightTempCombo_->currentData().toString();
      if (!t.isEmpty()) vkl.insert("temperature", t);
    }
    if (virtualKeyLightPanSpin_) vkl.insert("pan", virtualKeyLightPanSpin_->value());
    if (virtualKeyLightHdriEdit_) {
      const QString p = virtualKeyLightHdriEdit_->text().trimmed();
      if (!p.isEmpty()) vkl.insert("hdri_path", p);
    }
    if (!vkl.isEmpty()) effects.insert("virtual_key_light", vkl);
  }

  // Vignette (GPU post-process)
  if (vignetteCheck_ || vignetteIntensitySlider_ || vignetteCenterOnFaceCheck_) {
    QJsonObject vg;
    if (vignetteCheck_) vg.insert("enabled", vignetteCheck_->isChecked());
    if (vignetteIntensitySlider_) vg.insert("intensity", vignetteIntensitySlider_->value());
    if (vignetteCenterOnFaceCheck_) vg.insert("center_on_face", vignetteCenterOnFaceCheck_->isChecked());
    if (!vg.isEmpty()) effects.insert("vignette", vg);
  }

  const QByteArray json = QJsonDocument(effects).toJson(QJsonDocument::Compact);
  const std::string req = std::string("SET_VIDEO_EFFECTS_JSON ") + json.toStdString();

  QString err;
  if (!DaemonRequest(req, nullptr, &err)) {
    ShowError("Effects update failed", err);
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
  (void)checked;
  (void)SendDaemonVideoEffects();
}

void VideoPage::OnBackgroundChanged(int /*index*/) {
  // Enable per-effect parameter controls.
  const QString bg = backgroundCombo_ ? backgroundCombo_->currentData().toString() : QString();
  if (backgroundStrengthSpin_) backgroundStrengthSpin_->setEnabled(bg == "blur");
  if (backgroundRemoveColorEdit_) backgroundRemoveColorEdit_->setEnabled(bg == "remove");
  if (backgroundReplaceImageEdit_) backgroundReplaceImageEdit_->setEnabled(bg == "replace");
  if (browseReplaceImageBtn_) browseReplaceImageBtn_->setEnabled(bg == "replace");
  (void)SendDaemonVideoEffects();
}

void VideoPage::OnBackgroundStrengthChanged(int /*value*/) {
  (void)SendDaemonVideoEffects();
}

void VideoPage::OnBackgroundRemoveColorChanged() {
  (void)SendDaemonVideoEffects();
}

void VideoPage::OnBackgroundReplaceImageChanged() {
  (void)SendDaemonVideoEffects();
}

void VideoPage::OnBrowseReplaceImage() {
  const QString file = QFileDialog::getOpenFileName(this,
                                                    "Select background image",
                                                    QString(),
                                                    "PPM (P6) Images (*.ppm *.PPM);;All files (*)");
  if (file.isEmpty()) return;
  if (backgroundReplaceImageEdit_) backgroundReplaceImageEdit_->setText(file);
  (void)SendDaemonVideoEffects();
}

void VideoPage::OnAutoFrameToggled(bool /*checked*/) {
  UpdateUiEnabled();
  (void)SendDaemonVideoEffects();
}

void VideoPage::OnAutoFrameZoomChanged(int value) {
  if (autoFrameZoomValue_) autoFrameZoomValue_->setText(QString::number(value) + "%");
  (void)SendDaemonVideoEffects();
}

void VideoPage::OnEyeContactToggled(bool /*checked*/) {
  UpdateUiEnabled();
  (void)SendDaemonVideoEffects();
}

void VideoPage::OnEyeContactStrengthChanged(int value) {
  if (eyeContactStrengthValue_) eyeContactStrengthValue_->setText(QString::number(value) + "%");
  (void)SendDaemonVideoEffects();
}

void VideoPage::OnEyeContactLookAwayToggled(bool /*checked*/) {
  (void)SendDaemonVideoEffects();
}

void VideoPage::OnDenoiseToggled(bool /*checked*/) {
  UpdateUiEnabled();
  (void)SendDaemonVideoEffects();
}

void VideoPage::OnDenoiseStrengthChanged(int value) {
  if (denoiseStrengthValue_) denoiseStrengthValue_->setText(QString::number(value) + "%");
  (void)SendDaemonVideoEffects();
}

void VideoPage::OnOpenInstallHints() {
  QString program = QCoreApplication::applicationDirPath() + "/studiocast-maxine";
  if (!QFileInfo::exists(program)) {
    program = "studiocast-maxine";
  }

  QProcess p;
  p.setProgram(program);
  p.setArguments({"install-hints"});
  p.start();
  if (!p.waitForFinished(15000)) {
    p.kill();
    p.waitForFinished(2000);
  }

  const QString out = QString::fromUtf8(p.readAllStandardOutput());
  const QString err = QString::fromUtf8(p.readAllStandardError());
  const QString text = (out + (err.isEmpty() ? "" : ("\n" + err))).trimmed();

  QMessageBox mb(this);
  mb.setWindowTitle("Maxine install hints");
  mb.setText(text.isEmpty() ? "No output." : "See details.");
  mb.setDetailedText(text.isEmpty() ? QString() : text);
  mb.exec();
}

void VideoPage::OnVirtualKeyLightToggled(bool /*checked*/) {
  UpdateUiEnabled();
  (void)SendDaemonVideoEffects();
}

void VideoPage::OnVirtualKeyLightIntensityChanged(int /*value*/) {
  (void)SendDaemonVideoEffects();
}

void VideoPage::OnVirtualKeyLightTemperatureChanged(int /*index*/) {
  (void)SendDaemonVideoEffects();
}

void VideoPage::OnVirtualKeyLightPanChanged(int /*value*/) {
  (void)SendDaemonVideoEffects();
}

void VideoPage::OnVirtualKeyLightHdriChanged() {
  (void)SendDaemonVideoEffects();
}

void VideoPage::OnBrowseVirtualKeyLightHdri() {
  const QString file = QFileDialog::getOpenFileName(this,
                                                    "Select HDRI (.hdr/.exr)",
                                                    QString(),
                                                    "HDRI (*.hdr *.exr);;All files (*)");
  if (file.isEmpty()) return;
  if (virtualKeyLightHdriEdit_) virtualKeyLightHdriEdit_->setText(file);
  (void)SendDaemonVideoEffects();
}

void VideoPage::OnVignetteToggled(bool checked) {
  (void)checked;
  (void)SendDaemonVideoEffects();
}

void VideoPage::OnVignetteIntensityChanged(int value) {
  if (vignetteIntensityValue_) vignetteIntensityValue_->setText(QString::number(value) + "%");
  (void)SendDaemonVideoEffects();
}

void VideoPage::OnVignetteCenterOnFaceToggled(bool checked) {
  (void)checked;
  (void)SendDaemonVideoEffects();
}

void VideoPage::OnStart() {
  if (!SendDaemonVideoConfig()) {
    UpdateStatusText();
    UpdateUiEnabled();
    return;
  }

  (void)SendDaemonVideoEffects();
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
  const bool maxineSupported = daemonReachable_ && st.maxine_supported;

  refreshBtn_->setEnabled(!enabled);
  copyCmdBtn_->setEnabled(!suggestedCmd_.isEmpty());

  const bool outSelectable = outputCombo_->count() > 0 && !outputCombo_->itemData(0).toString().isEmpty();

  inputCombo_->setEnabled(!enabled);
  outputCombo_->setEnabled(!enabled && outSelectable);

  widthSpin_->setEnabled(!enabled);
  heightSpin_->setEnabled(!enabled);
  fpsSpin_->setEnabled(!enabled);

  // Maxine blocking banner + diagnostics (best-effort)
  if (maxineBanner_) {
    if (daemonReachable_ && !st.maxine_supported) {
      QString msg = "Maxine unavailable.";
      if (!st.maxine_blocked_reason.isEmpty()) msg += "\n" + st.maxine_blocked_reason;
      if (!st.maxine_blocked_details.isEmpty()) {
        msg += "\n\n";
        for (const auto& d : st.maxine_blocked_details) {
          msg += "• " + d + "\n";
        }
      }
      msg += "\nEffects are disabled. Open Diagnostics for install/path hints.";
      maxineBanner_->setText(msg.trimmed());
      maxineBanner_->setVisible(true);
    } else {
      maxineBanner_->setVisible(false);
    }
  }

  if (diagnosticsText_ && daemonReachable_ && !daemonLastStatusJson_.empty()) {
    QJsonObject root;
    QString perr;
    if (ParseJsonObject(daemonLastStatusJson_, &root, &perr)) {
      const QJsonObject maxine = root.value("maxine").toObject();
      diagnosticsText_->setPlainText(QString::fromUtf8(QJsonDocument(maxine).toJson(QJsonDocument::Indented)));
    } else {
      diagnosticsText_->setPlainText("(failed to parse status JSON)\n" + perr);
    }
  }

  // Effects are only editable when Maxine is supported.
  const bool fxAvail = maxineSupported;
  auto hasFx = [&](const QString& id) {
    if (!fxAvail) return false;
    if (st.maxine_available_effects.isEmpty()) return true;  // treat as unknown → allow
    return st.maxine_available_effects.contains(id);
  };

  // Mirror
  if (mirrorCheck_) mirrorCheck_->setEnabled(fxAvail);

  // Virtual Background
  if (backgroundCombo_) backgroundCombo_->setEnabled(fxAvail);
  if (backgroundStrengthSpin_ && backgroundCombo_) {
    backgroundStrengthSpin_->setEnabled(fxAvail && backgroundCombo_->currentData().toString() == "blur");
  }
  if (backgroundRemoveColorEdit_ && backgroundCombo_) {
    backgroundRemoveColorEdit_->setEnabled(fxAvail && backgroundCombo_->currentData().toString() == "remove");
  }
  if (backgroundReplaceImageEdit_ && backgroundCombo_) {
    const bool on = fxAvail && backgroundCombo_->currentData().toString() == "replace";
    backgroundReplaceImageEdit_->setEnabled(on);
    if (browseReplaceImageBtn_) browseReplaceImageBtn_->setEnabled(on);
  }

  // Auto Frame
  const bool afAvail = hasFx("auto_frame");
  const bool afOn = autoFrameCheck_ ? autoFrameCheck_->isChecked() : false;
  if (autoFrameCheck_) autoFrameCheck_->setEnabled(afAvail);
  if (autoFrameZoomSlider_) autoFrameZoomSlider_->setEnabled(afAvail && afOn);

  // Eye Contact
  const bool ecAvail = hasFx("eye_contact");
  const bool ecOn = eyeContactCheck_ ? eyeContactCheck_->isChecked() : false;
  if (eyeContactCheck_) eyeContactCheck_->setEnabled(ecAvail);
  if (eyeContactStrengthSlider_) eyeContactStrengthSlider_->setEnabled(ecAvail && ecOn);
  if (eyeContactLookAwayCheck_) eyeContactLookAwayCheck_->setEnabled(ecAvail && ecOn);

  // Video Noise Removal
  const bool dnAvail = hasFx("video_noise_removal");
  const bool dnOn = denoiseCheck_ ? denoiseCheck_->isChecked() : false;
  if (denoiseCheck_) denoiseCheck_->setEnabled(dnAvail);
  if (denoiseStrengthSlider_) denoiseStrengthSlider_->setEnabled(dnAvail && dnOn);

  // Virtual Key Light gating based on Maxine diagnostics.
  const bool vklAvail = fxAvail && st.virtual_key_light_available;
  const bool vklOn = virtualKeyLightCheck_ ? virtualKeyLightCheck_->isChecked() : false;

  if (virtualKeyLightCheck_) virtualKeyLightCheck_->setEnabled(vklAvail);
  if (virtualKeyLightIntensitySpin_) virtualKeyLightIntensitySpin_->setEnabled(vklAvail && vklOn);
  if (virtualKeyLightTempCombo_) virtualKeyLightTempCombo_->setEnabled(vklAvail && vklOn);
  if (virtualKeyLightPanSpin_) virtualKeyLightPanSpin_->setEnabled(vklAvail && vklOn);
  if (virtualKeyLightHdriEdit_) virtualKeyLightHdriEdit_->setEnabled(vklAvail && vklOn);
  if (browseVirtualKeyLightHdriBtn_) browseVirtualKeyLightHdriBtn_->setEnabled(vklAvail && vklOn);

  // Vignette runs in the GPU pipeline; we only expose it when Maxine is supported.
  const bool vigAvail = fxAvail;
  const bool vigOn = vignetteCheck_ ? vignetteCheck_->isChecked() : false;
  if (vignetteCheck_) vignetteCheck_->setEnabled(vigAvail);
  if (vignetteIntensitySlider_) vignetteIntensitySlider_->setEnabled(vigAvail && vigOn);
  if (vignetteCenterOnFaceCheck_) vignetteCenterOnFaceCheck_->setEnabled(vigAvail && vigOn);

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
  oss << "  background: " << st.background.toStdString();
  if (!st.background_backend.isEmpty()) {
    oss << " (backend " << st.background_backend.toStdString() << ")";
  }
  if (st.background_strength > 0) {
    oss << " strength=" << st.background_strength;
  }
  if (st.background == "remove" && !st.background_remove_color.isEmpty()) {
    oss << " color=" << st.background_remove_color.toStdString();
  }
  if (st.background == "replace" && !st.background_replace_image.isEmpty()) {
    oss << " image=" << st.background_replace_image.toStdString();
  }
  oss << "\n";

  if (!st.maxine_summary.isEmpty()) {
    oss << "  maxine:     " << st.maxine_summary.toStdString() << "\n";
    oss << "  vkl avail:  " << (st.virtual_key_light_available ? "yes" : "no") << "\n";
  }

  oss << "  key light:  " << (st.virtual_key_light ? "on" : "off")
      << " intensity=" << st.virtual_key_light_intensity << "%"
      << " temp=" << st.virtual_key_light_temperature.toStdString()
      << " pan=" << st.virtual_key_light_pan << "\n";

  if (!st.effects_backends.isEmpty()) {
    oss << "  effects:    " << st.effects_backends.toStdString() << "\n";
  }
  if (!st.effects_note.isEmpty()) {
    oss << "  fx note:    " << st.effects_note.toStdString() << "\n";
  }
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
