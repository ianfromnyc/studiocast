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
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPaintEvent>
#include <QPainter>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSizePolicy>
#include <QSlider>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <sstream>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <unordered_map>
#include <vector>

#include "core/maxine/reason_codes.h"
#include "core/open_video/model_pack_registry.h"
#include "core/video/broadcast_camera_effects_json.h"
#include "core/video/convert.h"
#include "core/video/effects/broadcast_effect_contract.h"
#include "core/video/effects/effect_descriptors.h"
#include "core/video/v4l2loopback.h"

namespace studiocast::gui {

namespace {

// Debug helper for diagnosing preview start/stop behavior.
// Enable with: STUDIOCAST_DEBUG_GUI_PREVIEW=1
bool DebugGuiPreview() {
  static const bool enabled = (std::getenv("STUDIOCAST_DEBUG_GUI_PREVIEW") != nullptr);
  return enabled;
}

void GuiPreviewDbg(const std::string& msg) {
  if (!DebugGuiPreview()) return;
  std::cerr << "[gui_preview_dbg] " << msg << "\n";
}

}  // namespace



class VideoPreviewWidget final : public QWidget {
 public:
  explicit VideoPreviewWidget(QWidget* parent = nullptr) : QWidget(parent) {
    setMinimumSize(320, 240);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  }

  void SetStatusText(QString text) {
    frame_ = QImage{};
    statusText_ = std::move(text);
    update();
  }

  void SetFrame(const QImage& frame) {
    // Detach from the caller's backing buffer.
    frame_ = frame.copy();
    statusText_.clear();
    update();
  }

 protected:
  void paintEvent(QPaintEvent* /*event*/) override {
    QPainter p(this);
    // Paint using palette colors to match the active global theme.
    p.fillRect(rect(), palette().color(QPalette::Base));

    p.setPen(palette().color(QPalette::Mid));
    p.drawRect(rect().adjusted(0, 0, -1, -1));

    const QRect content = rect().adjusted(8, 8, -8, -8);

    if (!frame_.isNull()) {
      QSize scaled = frame_.size();
      scaled.scale(content.size(), Qt::KeepAspectRatio);
      QRect target(QPoint(0, 0), scaled);
      target.moveCenter(content.center());
      p.drawImage(target, frame_);
      return;
    }

    p.setPen(palette().color(QPalette::Text));
    const QString text = statusText_.isEmpty() ? QStringLiteral("Preview") : statusText_;
    p.drawText(content, Qt::AlignCenter | Qt::TextWordWrap, text);
  }

  QSize sizeHint() const override { return {640, 360}; }

 private:
  QImage frame_;
  QString statusText_;
};

namespace {

QString DeviceLabel(const studiocast::video::VideoDevice& d) {
  QString label = QString::fromStdString(d.dev_node);
  if (!d.name.empty()) label += " — " + QString::fromStdString(d.name);
  if (!d.driver.empty()) label += " (" + QString::fromStdString(d.driver) + ")";
  if (d.is_loopback) label += " [loopback]";
  return label;
}

enum class EffectAvailabilityKind {
  always,
  gpu_utility,
  maxine_listed,
};

const std::unordered_map<std::string, studiocast::video::effects::VideoEffectDescriptor>& EffectDescriptorById() {
  static const auto* map = []() {
    auto* m = new std::unordered_map<std::string, studiocast::video::effects::VideoEffectDescriptor>();
    for (const auto& d : studiocast::video::effects::VideoEffectDescriptors()) {
      (*m)[d.id] = d;
    }
    return m;
  }();
  return *map;
}

EffectAvailabilityKind AvailabilityKindForEffectId(const QString& id) {
  const auto& m = EffectDescriptorById();
  const auto it = m.find(id.toStdString());
  if (it == m.end()) {
    // Conservative default for unknown IDs.
    return EffectAvailabilityKind::maxine_listed;
  }
  bool needsMaxine = false;
  bool needsGpuUtility = false;
  for (const auto c : it->second.required_components) {
    needsMaxine = needsMaxine || (c == studiocast::video::effects::RequiredComponent::maxine_vfx ||
                                 c == studiocast::video::effects::RequiredComponent::maxine_ar);
    needsGpuUtility = needsGpuUtility || (c == studiocast::video::effects::RequiredComponent::gpu_utility);
  }
  if (needsMaxine) return EffectAvailabilityKind::maxine_listed;
  if (needsGpuUtility) return EffectAvailabilityKind::gpu_utility;
  return EffectAvailabilityKind::always;
}

struct DaemonVideoStatus {
  struct NegotiatedFormat {
    bool present = false;
    QString pixfmt;
    int width = 0;
    int height = 0;
    double fps = 0.0;
    int fps_num = 0;
    int fps_den = 0;
    int bytes_per_line = 0;
    int size_image = 0;
  };

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

  NegotiatedFormat capture_format;
  NegotiatedFormat output_format;

  // Output scaling info (from daemon status).
  QString scaling_backend_active;
  NegotiatedFormat scaling_from;
  NegotiatedFormat scaling_to;

  studiocast::video::effects::BroadcastCameraEffects effects{};
  bool effects_valid = false;

  // Rule-based disable reasons and deterministic ordering (from daemon status).
  QStringList effects_plan_ordered;
  QString effects_plan_vignette_attach_to;
  QMap<QString, QString> effects_plan_disabled;

  // Maxine runtime diagnostics (from daemon GET_STATUS)
  bool maxine_ok = false;
  bool maxine_supported = false;
  QString maxine_summary;
  QString maxine_blocked_reason;
  QStringList maxine_blocked_details;
  QStringList maxine_available_effects;
  QMap<QString, QStringList> maxine_missing_effects;
  bool virtual_key_light_available = false;

  // Open CUDA runtime diagnostics (from daemon GET_STATUS)
  bool open_cuda_present = false;
  bool open_cuda_ok = false;
  QString open_cuda_default_model_id;

  struct OpenCudaModelInfo {
    QString id;
    QString display_name;
    QString task;
    int width = 0;
    int height = 0;
  };
  std::vector<OpenCudaModelInfo> open_cuda_models;

  QStringList open_cuda_installed_models;
  QMap<QString, QString> open_cuda_missing_models;
  QStringList open_cuda_available_effects;
  QMap<QString, QString> open_cuda_blocked_effects;
  QStringList open_cuda_install_hints;

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

  const auto parseFormat = [](const QJsonObject& fmt) -> DaemonVideoStatus::NegotiatedFormat {
    DaemonVideoStatus::NegotiatedFormat f;
    if (fmt.isEmpty()) return f;

    f.present = true;
    f.pixfmt = fmt.value("pixfmt").toString();
    f.width = fmt.value("width").toInt(0);
    f.height = fmt.value("height").toInt(0);
    f.fps = fmt.value("fps").toDouble(0);
    f.fps_num = fmt.value("fps_num").toInt(0);
    f.fps_den = fmt.value("fps_den").toInt(0);
    f.bytes_per_line = fmt.value("bytesperline").toInt(0);
    f.size_image = fmt.value("sizeimage").toInt(0);
    return f;
  };

  out->capture_format = parseFormat(video.value("capture_format").toObject());
  out->output_format = parseFormat(video.value("output_format").toObject());

  const QJsonObject scaling = video.value("scaling").toObject();
  if (!scaling.isEmpty()) {
    out->scaling_backend_active = scaling.value("backend_active").toString();
    out->scaling_from = parseFormat(scaling.value("from").toObject());
    out->scaling_to = parseFormat(scaling.value("to").toObject());
  } else {
    out->scaling_backend_active.clear();
    out->scaling_from = {};
    out->scaling_to = {};
  }

  out->input_device = video.value("input_device").toString();
  out->output_device = video.value("output_device").toString();

  const QJsonObject pipe = video.value("pipeline").toObject();
  out->pipeline_running = pipe.value("running").toBool(false);
  out->pipeline_starting = pipe.value("starting").toBool(false);
  out->frame_index = static_cast<long long>(pipe.value("frame_index").toDouble(0));

  out->effects_backends = pipe.value("effects_backends").toString();
  out->effects_note = pipe.value("effects_note").toString();

  // Canonical effects model (Broadcast schema).
  out->effects = {};
  out->effects_valid = false;
  const QJsonObject fx = video.value("video_effects").toObject();
  if (!fx.isEmpty()) {
    const QByteArray txt = QJsonDocument(fx).toJson(QJsonDocument::Compact);
    std::string jerr;
    if (studiocast::video::ApplyBroadcastCameraEffectsPatchJsonText(txt.toStdString(), &out->effects, &jerr)) {
      out->effects_valid = true;
    }
  }

  // Effect plan (ordering + disable reasons).
  out->effects_plan_ordered.clear();
  out->effects_plan_vignette_attach_to.clear();
  out->effects_plan_disabled.clear();
  const QJsonObject plan = pipe.value("effects_plan").toObject();
  if (!plan.isEmpty()) {
    const auto ordered = plan.value("ordered").toArray();
    for (const auto& v : ordered) {
      const QString s = v.toString();
      if (!s.isEmpty()) out->effects_plan_ordered.push_back(s);
    }

    out->effects_plan_vignette_attach_to = plan.value("vignette_attach_to").toString();

    const auto disabled = plan.value("disabled").toArray();
    for (const auto& v : disabled) {
      const QJsonObject o = v.toObject();
      const QString id = o.value("id").toString();
      const QString reason = o.value("reason").toString();
      if (!id.isEmpty() && !reason.isEmpty()) {
        out->effects_plan_disabled.insert(id, reason);
      }
    }
  }

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
    out->maxine_missing_effects.clear();
    out->virtual_key_light_available = false;
    const auto arr = maxine.value("available_effects").toArray();
    for (const auto& v : arr) {
      const QString id = v.toString();
      if (!id.isEmpty()) out->maxine_available_effects.push_back(id);
      if (id == "virtual_key_light") {
        out->virtual_key_light_available = true;
      }
    }

    const QJsonObject missing = maxine.value("missing_effects").toObject();
    for (auto it = missing.begin(); it != missing.end(); ++it) {
      const QString id = it.key();
      if (id.isEmpty()) continue;
      QStringList reasons;
      const auto reasonArr = it.value().toArray();
      for (const auto& rv : reasonArr) {
        const QString s = rv.toString();
        if (!s.isEmpty()) reasons.push_back(s);
      }
      out->maxine_missing_effects.insert(id, reasons);
    }
  }

  // Open CUDA diagnostics payload.
  out->open_cuda_present = false;
  out->open_cuda_ok = false;
  out->open_cuda_default_model_id.clear();
  out->open_cuda_models.clear();
  out->open_cuda_installed_models.clear();
  out->open_cuda_missing_models.clear();
  out->open_cuda_available_effects.clear();
  out->open_cuda_blocked_effects.clear();
  out->open_cuda_install_hints.clear();

  QJsonObject openCuda = root.value("open_cuda").toObject();
  if (openCuda.isEmpty()) {
    const QJsonObject engines = root.value("engines").toObject();
    openCuda = engines.value("open_cuda").toObject();
  }
  if (!openCuda.isEmpty()) {
    out->open_cuda_present = true;
    out->open_cuda_ok = openCuda.value("ok").toBool(false);

    out->open_cuda_default_model_id = openCuda.value("default_model_id").toString();

    const auto models = openCuda.value("models").toArray();
    for (const auto& v : models) {
      const QJsonObject o = v.toObject();
      const QString id = o.value("id").toString();
      if (id.isEmpty()) continue;
      DaemonVideoStatus::OpenCudaModelInfo mi;
      mi.id = id;
      mi.display_name = o.value("display_name").toString(id);
      mi.task = o.value("task").toString();
      mi.width = o.value("width").toInt(0);
      mi.height = o.value("height").toInt(0);
      out->open_cuda_models.push_back(mi);
    }

    const auto installed = openCuda.value("installed_models").toArray();
    for (const auto& v : installed) {
      const QString s = v.toString();
      if (!s.isEmpty()) out->open_cuda_installed_models.push_back(s);
    }

    if (out->open_cuda_installed_models.isEmpty() && !out->open_cuda_models.empty()) {
      for (const auto& m : out->open_cuda_models) {
        if (!m.id.isEmpty()) out->open_cuda_installed_models.push_back(m.id);
      }
    }

    // Backward compatibility: older daemons only provide installed model IDs.
    if (out->open_cuda_models.empty() && !out->open_cuda_installed_models.isEmpty()) {
      out->open_cuda_models.reserve(static_cast<std::size_t>(out->open_cuda_installed_models.size()));
      for (const auto& id : out->open_cuda_installed_models) {
        DaemonVideoStatus::OpenCudaModelInfo mi;
        mi.id = id;
        mi.display_name = id;
        out->open_cuda_models.push_back(mi);
      }
    }

    const QJsonObject missing = openCuda.value("missing_models").toObject();
    for (auto it = missing.begin(); it != missing.end(); ++it) {
      const QString id = it.key();
      const QString reason = it.value().toString();
      if (!id.isEmpty() && !reason.isEmpty()) {
        out->open_cuda_missing_models.insert(id, reason);
      }
    }

    const auto available = openCuda.value("available_effects").toArray();
    for (const auto& v : available) {
      const QString id = v.toString();
      if (!id.isEmpty()) out->open_cuda_available_effects.push_back(id);
    }

    const QJsonObject blocked = openCuda.value("blocked_effects").toObject();
    for (auto it = blocked.begin(); it != blocked.end(); ++it) {
      const QString id = it.key();
      const QString reason = it.value().toString();
      if (!id.isEmpty() && !reason.isEmpty()) {
        out->open_cuda_blocked_effects.insert(id, reason);
      }
    }

    const auto hints = openCuda.value("install_hints").toArray();
    for (const auto& v : hints) {
      const QString s = v.toString();
      if (!s.isEmpty()) out->open_cuda_install_hints.push_back(s);
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

}  // namespace

VideoPage::VideoPage(QWidget* parent) : QWidget(parent) {
  auto* root = new QVBoxLayout(this);
  root->setSpacing(12);

  auto* title = new QLabel("Camera", this);
  title->setProperty("scRole", "title");
  root->addWidget(title);

  auto* box = new QGroupBox("Processed Camera → Virtual Camera (daemon-driven)", this);
  auto* boxLayout = new QVBoxLayout(box);

  // Preview
  preview_ = new VideoPreviewWidget(box);
  preview_->SetStatusText("Preview (opens the virtual camera as a consumer)");
  boxLayout->addWidget(preview_);

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

  // Effect engine preference (Auto / Maxine / Open CUDA)
  auto* engineRow = new QHBoxLayout();
  engineRow->addWidget(new QLabel("Effect engine:", box));
  engineCombo_ = new QComboBox(box);
  engineCombo_->addItem("Auto", "auto");
  engineCombo_->addItem("Maxine", "maxine");
  engineCombo_->addItem("Open CUDA", "open_cuda");
  engineRow->addWidget(engineCombo_);
  engineRow->addSpacing(12);
  engineRow->addWidget(new QLabel("Active:", box));
  effectEngineValue_ = new QLabel("—", box);
  effectEngineValue_->setProperty("scRole", "value");
  engineRow->addWidget(effectEngineValue_);
  engineRow->addStretch(1);
  boxLayout->addLayout(engineRow);

  maxineBanner_ = new QLabel(box);
  maxineBanner_->setWordWrap(true);
  maxineBanner_->setProperty("scBanner", "warning");
  maxineBanner_->setVisible(false);
  boxLayout->addWidget(maxineBanner_);

  // Mirror
  auto* fxRow = new QHBoxLayout();
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

  auto* vbModelRow = new QHBoxLayout();
  vbModelLabel_ = new QLabel("Model:", vbBox);
  vbModelCombo_ = new QComboBox(vbBox);
  vbModelCombo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
  vbModelRow->addWidget(vbModelLabel_);
  vbModelRow->addWidget(vbModelCombo_, 1);
  vbLayout->addLayout(vbModelRow);

  vbModelLabel_->setVisible(false);
  vbModelCombo_->setVisible(false);

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
  auto* afLayout = new QVBoxLayout(afBox);

  auto* afRow = new QHBoxLayout();
  autoFrameCheck_ = new QCheckBox("Enable", afBox);
  afRow->addWidget(autoFrameCheck_);
  afRow->addSpacing(12);
  afRow->addWidget(new QLabel("Zoom:", afBox));
  autoFrameZoomSlider_ = new QSlider(Qt::Horizontal, afBox);
  autoFrameZoomSlider_->setRange(0, 100);
  autoFrameZoomSlider_->setValue(50);
  afRow->addWidget(autoFrameZoomSlider_, 1);
  autoFrameZoomValue_ = new QLabel("50%", afBox);
  autoFrameZoomValue_->setMinimumWidth(44);
  autoFrameZoomValue_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  afRow->addWidget(autoFrameZoomValue_);
  afLayout->addLayout(afRow);

  auto* afModelRow = new QHBoxLayout();
  autoFrameModelLabel_ = new QLabel("Model:", afBox);
  autoFrameModelCombo_ = new QComboBox(afBox);
  autoFrameModelCombo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
  afModelRow->addWidget(autoFrameModelLabel_);
  afModelRow->addWidget(autoFrameModelCombo_, 1);
  afLayout->addLayout(afModelRow);
  autoFrameModelLabel_->setVisible(false);
  autoFrameModelCombo_->setVisible(false);
  boxLayout->addWidget(afBox);

  // Eye Contact
  auto* ecBox = new QGroupBox("Eye Contact", box);
  auto* ecLayout = new QVBoxLayout(ecBox);

  auto* ecRow = new QHBoxLayout();
  eyeContactCheck_ = new QCheckBox("Enable", ecBox);
  ecRow->addWidget(eyeContactCheck_);
  ecRow->addSpacing(12);
  ecRow->addWidget(new QLabel("Strength:", ecBox));
  eyeContactStrengthSlider_ = new QSlider(Qt::Horizontal, ecBox);
  eyeContactStrengthSlider_->setRange(0, 100);
  eyeContactStrengthSlider_->setValue(50);
  ecRow->addWidget(eyeContactStrengthSlider_, 1);
  eyeContactStrengthValue_ = new QLabel("50%", ecBox);
  eyeContactStrengthValue_->setMinimumWidth(44);
  eyeContactStrengthValue_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  ecRow->addWidget(eyeContactStrengthValue_);
  ecRow->addSpacing(12);
  eyeContactLookAwayCheck_ = new QCheckBox("Allow look-away", ecBox);
  eyeContactLookAwayCheck_->setChecked(true);
  ecRow->addWidget(eyeContactLookAwayCheck_);
  ecLayout->addLayout(ecRow);

  auto* ecModelRow = new QHBoxLayout();
  eyeContactModelLabel_ = new QLabel("Model:", ecBox);
  eyeContactModelCombo_ = new QComboBox(ecBox);
  eyeContactModelCombo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
  ecModelRow->addWidget(eyeContactModelLabel_);
  ecModelRow->addWidget(eyeContactModelCombo_, 1);
  ecLayout->addLayout(ecModelRow);
  eyeContactModelLabel_->setVisible(false);
  eyeContactModelCombo_->setVisible(false);
  boxLayout->addWidget(ecBox);

  // Video Noise Removal
  auto* dnBox = new QGroupBox("Video Noise Removal", box);
  auto* dnLayout = new QVBoxLayout(dnBox);

  auto* dnRow = new QHBoxLayout();
  denoiseCheck_ = new QCheckBox("Enable", dnBox);
  dnRow->addWidget(denoiseCheck_);
  dnRow->addSpacing(12);
  dnRow->addWidget(new QLabel("Strength:", dnBox));
  denoiseStrengthSlider_ = new QSlider(Qt::Horizontal, dnBox);
  denoiseStrengthSlider_->setRange(0, 100);
  denoiseStrengthSlider_->setValue(50);
  dnRow->addWidget(denoiseStrengthSlider_, 1);
  denoiseStrengthValue_ = new QLabel("50%", dnBox);
  denoiseStrengthValue_->setMinimumWidth(44);
  denoiseStrengthValue_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  dnRow->addWidget(denoiseStrengthValue_);
  dnLayout->addLayout(dnRow);

  auto* dnModelRow = new QHBoxLayout();
  denoiseModelLabel_ = new QLabel("Model:", dnBox);
  denoiseModelCombo_ = new QComboBox(dnBox);
  denoiseModelCombo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
  dnModelRow->addWidget(denoiseModelLabel_);
  dnModelRow->addWidget(denoiseModelCombo_, 1);
  dnLayout->addLayout(dnModelRow);
  denoiseModelLabel_->setVisible(false);
  denoiseModelCombo_->setVisible(false);
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

  connect(engineCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &VideoPage::OnEnginePreferenceChanged);

  connect(backgroundCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &VideoPage::OnBackgroundChanged);
  connect(vbModelCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &VideoPage::OnVbModelChanged);
  connect(backgroundStrengthSpin_, QOverload<int>::of(&QSpinBox::valueChanged),
          this, &VideoPage::OnBackgroundStrengthChanged);

  connect(backgroundRemoveColorEdit_, &QLineEdit::editingFinished, this, &VideoPage::OnBackgroundRemoveColorChanged);
  connect(backgroundReplaceImageEdit_, &QLineEdit::editingFinished, this, &VideoPage::OnBackgroundReplaceImageChanged);
  connect(browseReplaceImageBtn_, &QPushButton::clicked, this, &VideoPage::OnBrowseReplaceImage);

  connect(autoFrameCheck_, &QCheckBox::toggled, this, &VideoPage::OnAutoFrameToggled);
  connect(autoFrameZoomSlider_, &QSlider::valueChanged, this, &VideoPage::OnAutoFrameZoomChanged);
  connect(autoFrameModelCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &VideoPage::OnAutoFrameModelChanged);

  connect(eyeContactCheck_, &QCheckBox::toggled, this, &VideoPage::OnEyeContactToggled);
  connect(eyeContactStrengthSlider_, &QSlider::valueChanged, this, &VideoPage::OnEyeContactStrengthChanged);
  connect(eyeContactLookAwayCheck_, &QCheckBox::toggled, this, &VideoPage::OnEyeContactLookAwayToggled);
  connect(eyeContactModelCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &VideoPage::OnEyeContactModelChanged);

  connect(denoiseCheck_, &QCheckBox::toggled, this, &VideoPage::OnDenoiseToggled);
  connect(denoiseStrengthSlider_, &QSlider::valueChanged, this, &VideoPage::OnDenoiseStrengthChanged);
  connect(denoiseModelCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &VideoPage::OnDenoiseModelChanged);

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
  // `GET_CONFIG` is canonical effects-only; GUI sync uses `GET_STATUS` for full video config.
  if (!DaemonRequest("GET_STATUS", &json, &err)) {
    daemonReachable_ = false;
    return false;
  }

  daemonReachable_ = true;

  QJsonObject root;
  QString parseErr;
  if (!ParseJsonObject(json, &root, &parseErr)) {
    return false;
  }

  // Prefer `GET_STATUS.video` shape; keep best-effort fallback for older daemons.
  const QJsonObject video = root.value("video").toObject();
  const QJsonObject src = video.isEmpty() ? root : video;

  const QString input = src.value("input_device").toString();
  const QString output = src.value("output_device").toString();
  const int w = src.value("width").toInt(0);
  const int h = src.value("height").toInt(0);
  const int fps = src.value("fps").toInt(0);

  const QJsonObject fx = src.value("video_effects").toObject();

  // Canonical contract: `video_effects` is a patch object in Broadcast contract-ID form.
  // GUI does not read any legacy effect fields here.
  effects_ = {};
  if (!fx.isEmpty()) {
    const QByteArray txt = QJsonDocument(fx).toJson(QJsonDocument::Compact);
    std::string jerr;
    (void)studiocast::video::ApplyBroadcastCameraEffectsPatchJsonText(txt.toStdString(), &effects_, &jerr);
  }

  if (engineCombo_) {
    engineCombo_->blockSignals(true);
    const QString v = QString::fromStdString(studiocast::video::effects::ToString(effects_.engine));
    const int idx = engineCombo_->findData(v);
    engineCombo_->setCurrentIndex(idx >= 0 ? idx : 0);
    engineCombo_->blockSignals(false);
  }

  const bool autoFrame = effects_.auto_frame.enabled;
  const int autoFrameStrength = effects_.auto_frame.strength;
  const QString vbMode = autoFrame ? QStringLiteral("auto_frame")
                                  : QString::fromStdString(studiocast::video::effects::ToString(effects_.virtual_background.mode));
  const int vbStrength = effects_.virtual_background.strength;
  const QString vbRemoveColor = QString::fromStdString(effects_.virtual_background.remove_color);
  const QString vbReplacePath = QString::fromStdString(effects_.virtual_background.replace_path);

  const bool eyeContact = effects_.eye_contact.enabled;
  const int eyeContactStrength = effects_.eye_contact.strength;
  const bool eyeContactLookAway = effects_.eye_contact.look_away_enabled;

  const bool denoise = effects_.video_noise_removal.enabled;
  const int denoiseStrength = effects_.video_noise_removal.strength;

  const bool virtualKeyLight = effects_.virtual_key_light.enabled;
  const int virtualKeyLightIntensity = effects_.virtual_key_light.intensity;
  const auto vklPresetToStr = [](int p) -> const char* {
    switch (p) {
      case 1:
        return "warm";
      case 2:
        return "cool";
      default:
        return "neutral";
    }
  };
  const QString virtualKeyLightTemp = QString::fromUtf8(vklPresetToStr(effects_.virtual_key_light.temperature_preset));
  const int virtualKeyLightPan = effects_.virtual_key_light.direction_pan_degrees;
  const QString virtualKeyLightHdri = QString::fromStdString(effects_.virtual_key_light.hdri_path);

  const bool vignette = effects_.vignette.enabled;
  const int vignetteIntensity = effects_.vignette.intensity;
  const bool vignetteCenterOnFace = effects_.vignette.center_on_tracked_face;

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

  if (backgroundCombo_) {
    backgroundCombo_->blockSignals(true);
    const QString key = vbMode.isEmpty() ? "none" : vbMode;
    const int idx = backgroundCombo_->findData(key);
    if (idx >= 0) backgroundCombo_->setCurrentIndex(idx);
    backgroundCombo_->blockSignals(false);
  }

  if (backgroundStrengthSpin_) {
    backgroundStrengthSpin_->blockSignals(true);
    backgroundStrengthSpin_->setValue(std::max(0, std::min(100, vbStrength)));
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
    autoFrameZoomSlider_->setValue(std::max(0, std::min(100, autoFrameStrength)));
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
  // Canonical local model is `effects_` (Broadcast schema). Sync UI -> model here,
  // then serialize using the stable contract JSON.
  effects_.engine = studiocast::video::effects::EffectsEnginePreference::auto_select;
  if (engineCombo_) {
    const QString s = engineCombo_->currentData().toString();
    studiocast::video::effects::EffectsEnginePreference ep = effects_.engine;
    if (studiocast::video::effects::ParseEffectsEnginePreference(s.toStdString(), &ep)) {
      effects_.engine = ep;
    }
  }

  const QString bg = backgroundCombo_ ? backgroundCombo_->currentData().toString() : QString();
  const bool bgIsAutoFrame = (bg == "auto_frame");

  // Auto Frame vs Virtual Background are mutually exclusive in the current UX.
  effects_.auto_frame.enabled = (autoFrameCheck_ && autoFrameCheck_->isChecked()) || bgIsAutoFrame;
  if (autoFrameZoomSlider_) {
    effects_.auto_frame.strength = autoFrameZoomSlider_->value();
  }
  if (autoFrameModelCombo_ && autoFrameModelCombo_->count() > 0) {
    effects_.auto_frame.model_id = autoFrameModelCombo_->currentData().toString().toStdString();
  }

  // Virtual background mode.
  if (bgIsAutoFrame || effects_.auto_frame.enabled) {
    effects_.virtual_background.mode = studiocast::video::effects::VirtualBackgroundMode::none;
  } else {
    studiocast::video::effects::VirtualBackgroundMode m = studiocast::video::effects::VirtualBackgroundMode::none;
    (void)studiocast::video::effects::ParseVirtualBackgroundMode(bg.toStdString(), &m);
    effects_.virtual_background.mode = m;
  }
  if (vbModelCombo_ && vbModelCombo_->count() > 0) {
    effects_.virtual_background.model_id = vbModelCombo_->currentData().toString().toStdString();
  }
  if (backgroundStrengthSpin_) effects_.virtual_background.strength = backgroundStrengthSpin_->value();
  if (backgroundRemoveColorEdit_) effects_.virtual_background.remove_color = backgroundRemoveColorEdit_->text().trimmed().toStdString();
  if (backgroundReplaceImageEdit_) effects_.virtual_background.replace_path = backgroundReplaceImageEdit_->text().trimmed().toStdString();

  // Eye contact.
  if (eyeContactCheck_) effects_.eye_contact.enabled = eyeContactCheck_->isChecked();
  if (eyeContactStrengthSlider_) effects_.eye_contact.strength = eyeContactStrengthSlider_->value();
  if (eyeContactLookAwayCheck_) effects_.eye_contact.look_away_enabled = eyeContactLookAwayCheck_->isChecked();
  if (eyeContactModelCombo_ && eyeContactModelCombo_->count() > 0) {
    effects_.eye_contact.model_id = eyeContactModelCombo_->currentData().toString().toStdString();
  }

  // Denoise.
  if (denoiseCheck_) effects_.video_noise_removal.enabled = denoiseCheck_->isChecked();
  if (denoiseStrengthSlider_) effects_.video_noise_removal.strength = denoiseStrengthSlider_->value();
  if (denoiseModelCombo_ && denoiseModelCombo_->count() > 0) {
    effects_.video_noise_removal.model_id = denoiseModelCombo_->currentData().toString().toStdString();
  }

  // Virtual Key Light.
  if (virtualKeyLightCheck_) effects_.virtual_key_light.enabled = virtualKeyLightCheck_->isChecked();
  if (virtualKeyLightIntensitySpin_) effects_.virtual_key_light.intensity = virtualKeyLightIntensitySpin_->value();
  if (virtualKeyLightTempCombo_) {
    const QString t = virtualKeyLightTempCombo_->currentData().toString();
    if (t == "warm") effects_.virtual_key_light.temperature_preset = 1;
    else if (t == "cool") effects_.virtual_key_light.temperature_preset = 2;
    else effects_.virtual_key_light.temperature_preset = 0;
  }
  if (virtualKeyLightPanSpin_) effects_.virtual_key_light.direction_pan_degrees = virtualKeyLightPanSpin_->value();
  if (virtualKeyLightHdriEdit_) effects_.virtual_key_light.hdri_path = virtualKeyLightHdriEdit_->text().trimmed().toStdString();

  // Vignette.
  if (vignetteCheck_) effects_.vignette.enabled = vignetteCheck_->isChecked();
  if (vignetteIntensitySlider_) effects_.vignette.intensity = vignetteIntensitySlider_->value();
  if (vignetteCenterOnFaceCheck_) effects_.vignette.center_on_tracked_face = vignetteCenterOnFaceCheck_->isChecked();

  const std::string json = studiocast::video::BroadcastCameraEffectsContractToJson(effects_);
  const std::string req = std::string("SET_VIDEO_EFFECTS_JSON ") + json;

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

void VideoPage::OnEnginePreferenceChanged(int /*index*/) {
  UpdateUiEnabled();
  (void)SendDaemonVideoEffects();
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
  UpdateUiEnabled();
  (void)SendDaemonVideoEffects();
}

void VideoPage::OnVbModelChanged(int /*index*/) {
  if (!vbModelCombo_ || vbModelCombo_->count() <= 0) return;
  effects_.virtual_background.model_id = vbModelCombo_->currentData().toString().toStdString();
  (void)SendDaemonVideoEffects();
}

void VideoPage::OnAutoFrameModelChanged(int /*index*/) {
  if (!autoFrameModelCombo_ || autoFrameModelCombo_->count() <= 0) return;
  effects_.auto_frame.model_id = autoFrameModelCombo_->currentData().toString().toStdString();
  (void)SendDaemonVideoEffects();
}

void VideoPage::OnEyeContactModelChanged(int /*index*/) {
  if (!eyeContactModelCombo_ || eyeContactModelCombo_->count() <= 0) return;
  effects_.eye_contact.model_id = eyeContactModelCombo_->currentData().toString().toStdString();
  (void)SendDaemonVideoEffects();
}

void VideoPage::OnDenoiseModelChanged(int /*index*/) {
  if (!denoiseModelCombo_ || denoiseModelCombo_->count() <= 0) return;
  effects_.video_noise_removal.model_id = denoiseModelCombo_->currentData().toString().toStdString();
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
  studiocast::video::effects::EffectsEnginePreference pref =
      studiocast::video::effects::EffectsEnginePreference::auto_select;
  if (engineCombo_) {
    const QString s = engineCombo_->currentData().toString();
    (void)studiocast::video::effects::ParseEffectsEnginePreference(s.toStdString(), &pref);
  }

  const auto runHints = [&](const QString& program, const QString& title) -> QString {
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
    if (text.isEmpty()) return title + ": (no output)";
    return title + ":\n" + text;
  };

  const auto resolveProgram = [&](const char* exeName) -> QString {
    QString program = QCoreApplication::applicationDirPath() + "/" + exeName;
    if (QFileInfo::exists(program)) return program;
    return QString::fromUtf8(exeName);
  };

  QString text;
  QString title;
  if (pref == studiocast::video::effects::EffectsEnginePreference::open_cuda) {
    title = "Open CUDA install hints";
    text = runHints(resolveProgram("studiocast-open"), "studiocast-open");
  } else if (pref == studiocast::video::effects::EffectsEnginePreference::maxine) {
    title = "Maxine install hints";
    text = runHints(resolveProgram("studiocast-maxine"), "studiocast-maxine");
  } else {
    title = "Engine install hints";
    text = runHints(resolveProgram("studiocast-maxine"), "studiocast-maxine") + "\n\n" +
           runHints(resolveProgram("studiocast-open"), "studiocast-open");
  }

  QMessageBox mb(this);
  mb.setWindowTitle(title);
  mb.setText("See details.");
  mb.setDetailedText(text.trimmed());
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
  if (DebugGuiPreview()) {
    GuiPreviewDbg("OnStart clicked: sending config/effects and enabling video");
  }

  if (!SendDaemonVideoConfig()) {
    if (DebugGuiPreview()) {
      GuiPreviewDbg("OnStart: SendDaemonVideoConfig failed");
    }
    UpdateStatusText();
    UpdateUiEnabled();
    return;
  }

  (void)SendDaemonVideoEffects();
  (void)SendDaemonEnabled(true);

  if (DebugGuiPreview()) {
    GuiPreviewDbg("OnStart: daemon enabled=true; starting preview");
  }

  StartPreview();
  UpdateStatusText();
  UpdateUiEnabled();
}

void VideoPage::OnStop() {
  if (DebugGuiPreview()) {
    GuiPreviewDbg("OnStop clicked: stopping preview and disabling video");
  }
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
  if (DebugGuiPreview()) {
    GuiPreviewDbg("StartPreview: begin");
  }
  StopPreview();

  // Determine which device to open for preview.
  QString outDev = outputCombo_->currentData().toString();
  int wantW = widthSpin_->value();
  int wantH = heightSpin_->value();
  int wantFps = fpsSpin_->value();
  std::optional<studiocast::video::CapturePixelFormat> wantFmt;

  // Ask daemon for the resolved output device and negotiated output format.
  //
  // Using the daemon's negotiated format avoids capture-side renegotiation on
  // v4l2loopback devices (which can destabilize the producer and cause start/stop
  // thrashing).
  {
    std::string json;
    QString err;
    if (DaemonRequest("GET_STATUS", &json, &err)) {
      DaemonVideoStatus st;
      QString perr;
      if (ParseDaemonStatusJson(json, &st, &perr)) {
        if (DebugGuiPreview()) {
          std::ostringstream oss;
          oss << "StartPreview: GET_STATUS enabled=" << (st.enabled ? 1 : 0)
              << " consumer_count=" << st.consumer_count
              << " output_device='" << st.output_device.toStdString() << "'"
              << " output_fmt=" << st.output_format.pixfmt.toStdString()
              << " " << st.output_format.width << "x" << st.output_format.height
              << " fps=" << st.output_format.fps;
          GuiPreviewDbg(oss.str());
        }
        if ((outDev.isEmpty() || outDev == "auto") && !st.output_device.isEmpty()) {
          outDev = st.output_device;
        }

        const bool outMatches = (!st.output_device.isEmpty() && outDev == st.output_device);
        if (outMatches && !st.output_format.pixfmt.isEmpty() && st.output_format.width > 0 && st.output_format.height > 0) {
          wantW = st.output_format.width;
          wantH = st.output_format.height;
          if (st.output_format.fps > 0.0) {
            wantFps = std::max(1, static_cast<int>(std::floor(st.output_format.fps + 0.5)));
          }

          if (st.output_format.pixfmt == "RGB3") {
            wantFmt = studiocast::video::CapturePixelFormat::rgb24;
          } else if (st.output_format.pixfmt == "YUYV") {
            wantFmt = studiocast::video::CapturePixelFormat::yuyv;
          }
        }
      }
    }
  }

  if (outDev.isEmpty() || outDev == "auto") {
    if (DebugGuiPreview()) {
      GuiPreviewDbg("StartPreview: no output device selected (outDev empty/auto)");
    }
    preview_->SetStatusText("Preview unavailable (no output device selected)");
    return;
  }

  const auto openFmt = [&](studiocast::video::CapturePixelFormat fmt, std::string* outErr) -> bool {
    return previewCapture_.Open(outDev.toStdString(), wantW, wantH, wantFps, fmt, false, outErr);
  };

  // Prefer the daemon's negotiated output format when available.
  const auto firstFmt = wantFmt.value_or(studiocast::video::CapturePixelFormat::rgb24);
  const auto secondFmt = (firstFmt == studiocast::video::CapturePixelFormat::rgb24)
                             ? studiocast::video::CapturePixelFormat::yuyv
                             : studiocast::video::CapturePixelFormat::rgb24;

  std::string err;
  if (!openFmt(firstFmt, &err)) {
    std::string err2;
    if (!openFmt(secondFmt, &err2)) {
      if (DebugGuiPreview()) {
        GuiPreviewDbg(std::string("StartPreview: previewCapture_.Open failed: ") + err2);
      }
      preview_->SetStatusText("Preview open failed:\n" + QString::fromStdString(err2));
      return;
    }
  }

  const auto fmt = previewCapture_.Actual();
  previewW_ = fmt.width;
  previewH_ = fmt.height;
  previewBpl_ = previewW_ * 3;
  previewRgb_.assign(static_cast<std::size_t>(previewBpl_ * previewH_), 0);

  previewTimer_->start();
  preview_->SetStatusText("Preview starting...");

  if (DebugGuiPreview()) {
    const auto a = previewCapture_.Actual();
    std::ostringstream oss;
    oss << "StartPreview: Open OK dev='" << outDev.toStdString() << "'"
        << " fmt=" << a.pixfmt
        << " " << a.width << "x" << a.height
        << " fps=" << a.fps
        << " bpl=" << a.bytes_per_line
        << " size=" << a.size_image;
    GuiPreviewDbg(oss.str());
  }
}

void VideoPage::StopPreview() {
  if (DebugGuiPreview()) {
    GuiPreviewDbg(std::string("StopPreview: timer=") +
                 ((previewTimer_ && previewTimer_->isActive()) ? "active" : "stopped") +
                 " capture_open=" + (previewCapture_.IsOpen() ? std::string("yes") : std::string("no")));
  }
  if (previewTimer_) previewTimer_->stop();
  if (previewCapture_.IsOpen()) previewCapture_.Close();
  previewRgb_.clear();
  previewW_ = previewH_ = previewBpl_ = 0;

  if (preview_) preview_->SetStatusText("Preview stopped");
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
  preview_->SetFrame(img);
}

void VideoPage::UpdateUiEnabled() {
  // Query daemon status (best-effort) to determine whether controls should be editable.
  DaemonVideoStatus st;
  if (daemonReachable_ && !daemonLastStatusJson_.empty()) {
    QString perr;
    (void)ParseDaemonStatusJson(daemonLastStatusJson_, &st, &perr);
  }

  const auto currentEnginePref = [&]() -> studiocast::video::effects::EffectsEnginePreference {
    studiocast::video::effects::EffectsEnginePreference ep = studiocast::video::effects::EffectsEnginePreference::auto_select;
    if (!engineCombo_) return ep;
    const QString s = engineCombo_->currentData().toString();
    (void)studiocast::video::effects::ParseEffectsEnginePreference(s.toStdString(), &ep);
    return ep;
  };

  const bool enabled = daemonReachable_ ? st.enabled : false;
  const bool maxineSupported = daemonReachable_ && st.maxine_supported;
  const bool openCudaSupported = daemonReachable_ && st.open_cuda_present && st.open_cuda_ok;
  const auto enginePref = currentEnginePref();

  refreshBtn_->setEnabled(!enabled);
  copyCmdBtn_->setEnabled(!suggestedCmd_.isEmpty());

  const bool outSelectable = outputCombo_->count() > 0 && !outputCombo_->itemData(0).toString().isEmpty();

  inputCombo_->setEnabled(!enabled);
  outputCombo_->setEnabled(!enabled && outSelectable);

  widthSpin_->setEnabled(!enabled);
  heightSpin_->setEnabled(!enabled);
  fpsSpin_->setEnabled(!enabled);

  if (engineCombo_) {
    engineCombo_->setEnabled(daemonReachable_);
  }

  if (effectEngineValue_) {
    if (!daemonReachable_) {
      effectEngineValue_->setText("—");
    } else if (!st.effects_backends.isEmpty()) {
      effectEngineValue_->setText(st.effects_backends);
    } else {
      effectEngineValue_->setText(QString::fromStdString(studiocast::video::effects::ToString(enginePref)));
    }
  }

  // Engine blocking banner + diagnostics (best-effort)
  if (maxineBanner_) {
    if (!daemonReachable_) {
      maxineBanner_->setVisible(false);
    } else {
      QString msg;
      bool show = false;

      const auto fmtMaxineBlocked = [&]() -> QString {
        QString s = "Maxine unavailable.";
        if (!st.maxine_blocked_reason.isEmpty()) {
          s += "\n" + FormatMaxineReasonCode(st.maxine_blocked_reason);
        }
        if (!st.maxine_blocked_details.isEmpty()) {
          s += "\n\n";
          for (const auto& d : st.maxine_blocked_details) {
            s += "• " + d + "\n";
          }
        }
        return s.trimmed();
      };

      const auto fmtOpenCudaBlocked = [&]() -> QString {
        QString s = "Open CUDA unavailable.";
        if (!st.open_cuda_present) {
          s += "\nDaemon did not report Open CUDA status (older studiocastd).";
        } else if (!st.open_cuda_ok) {
          if (st.open_cuda_installed_models.isEmpty()) {
            s += "\nNo usable Open CUDA model packs were found.";
          }
        }

        if (!st.open_cuda_missing_models.isEmpty()) {
          s += "\n\nMissing/invalid model packs:";
          for (auto it = st.open_cuda_missing_models.begin(); it != st.open_cuda_missing_models.end(); ++it) {
            s += "\n• " + it.key() + ": " + it.value();
          }
        }

        if (!st.open_cuda_install_hints.isEmpty()) {
          s += "\n\n" + st.open_cuda_install_hints.join("\n");
        } else {
          s += "\n\nModel packs: ~/.local/share/studiocast/models/open_video/<subject>/<pack_dir>/";
        }

        s += "\n\nRun: studiocast-open install-hints";
        return s.trimmed();
      };

      if (enginePref == studiocast::video::effects::EffectsEnginePreference::maxine) {
        if (!st.maxine_supported) {
          msg = fmtMaxineBlocked();
          msg += "\n\nEffects are disabled. Open Diagnostics for install/path hints.";
          show = true;
        }
      } else if (enginePref == studiocast::video::effects::EffectsEnginePreference::open_cuda) {
        if (!st.open_cuda_present || !st.open_cuda_ok) {
          msg = fmtOpenCudaBlocked();
          msg += "\n\nEffects are disabled. Open Diagnostics for details.";
          show = true;
        }
      } else {
        // auto_select
        if (!st.maxine_supported && !(st.open_cuda_present && st.open_cuda_ok)) {
          msg = "No effects engine is available.";
          msg += "\n\n" + fmtMaxineBlocked();
          msg += "\n\n" + fmtOpenCudaBlocked();
          show = true;
        }
      }

      maxineBanner_->setText(msg);
      maxineBanner_->setVisible(show);
    }
  }

  if (openInstallHintsBtn_) {
    if (enginePref == studiocast::video::effects::EffectsEnginePreference::open_cuda) {
      openInstallHintsBtn_->setText("Open CUDA install hints");
    } else if (enginePref == studiocast::video::effects::EffectsEnginePreference::maxine) {
      openInstallHintsBtn_->setText("Maxine install hints");
    } else {
      openInstallHintsBtn_->setText("Engine install hints");
    }
  }

  if (diagnosticsText_ && daemonReachable_ && !daemonLastStatusJson_.empty()) {
    QJsonObject root;
    QString perr;
    if (ParseJsonObject(daemonLastStatusJson_, &root, &perr)) {
      QJsonObject diag;
      if (enginePref == studiocast::video::effects::EffectsEnginePreference::open_cuda) {
        diag = root.value("open_cuda").toObject();
        if (diag.isEmpty()) diag = root.value("engines").toObject().value("open_cuda").toObject();
      } else if (enginePref == studiocast::video::effects::EffectsEnginePreference::maxine) {
        diag = root.value("maxine").toObject();
        if (diag.isEmpty()) diag = root.value("engines").toObject().value("maxine").toObject();
      } else {
        diag = root.value("engines").toObject();
      }

      QString note;
      if (enginePref == studiocast::video::effects::EffectsEnginePreference::open_cuda && !st.open_cuda_missing_models.isEmpty()) {
        note = QStringLiteral("NOTE: Some Open CUDA model packs are missing/invalid.\n");
        for (auto it = st.open_cuda_missing_models.begin(); it != st.open_cuda_missing_models.end(); ++it) {
          note += QStringLiteral("• ") + it.key() + QStringLiteral(": ") + it.value() + QChar('\n');
        }
        note = note.trimmed();
        note += QStringLiteral("\n\n");
      }

      diagnosticsText_->setPlainText(note + QString::fromUtf8(QJsonDocument(diag).toJson(QJsonDocument::Indented)));
    } else {
      diagnosticsText_->setPlainText("(failed to parse status JSON)\n" + perr);
    }
  }

  // Per-effect availability comes from daemon status:
  //  - `maxine.available_effects` / `maxine.missing_effects` (MaxineManager)
  //  - `pipeline.effects_plan.disabled` (rule-based gating; single source of truth)
  // Plus static effect descriptors that tell us which effects depend on Maxine.
  auto effectAvailable = [&](const QString& id) -> bool {
    if (!daemonReachable_) return false;
    if (st.effects_plan_disabled.contains(id)) return false;

    const auto kind = AvailabilityKindForEffectId(id);
    if (kind == EffectAvailabilityKind::always) return true;
    if (kind == EffectAvailabilityKind::gpu_utility) {
      // GPU utility effects require a CUDA-capable engine (Maxine or Open CUDA).
      return maxineSupported || openCudaSupported;
    }

    // Engine-specific availability for effects that are not always-on.
    if (enginePref == studiocast::video::effects::EffectsEnginePreference::maxine) {
      if (!maxineSupported) return false;
      if (st.maxine_available_effects.isEmpty() && st.maxine_missing_effects.isEmpty()) {
        // Disable-by-default when daemon did not report availability.
        return false;
      }
      return st.maxine_available_effects.contains(id);
    }

    if (enginePref == studiocast::video::effects::EffectsEnginePreference::open_cuda) {
      if (!(st.open_cuda_present && st.open_cuda_ok)) return false;
      return st.open_cuda_available_effects.contains(id);
    }

    // auto_select: prefer Maxine if available, else Open CUDA.
    if (maxineSupported && st.maxine_available_effects.contains(id)) return true;
    if (st.open_cuda_present && st.open_cuda_ok && st.open_cuda_available_effects.contains(id)) return true;
    return false;
  };

  auto effectUnavailableTooltip = [&](const QString& id) -> QString {
    if (!daemonReachable_) return "Daemon unreachable.";

    if (st.effects_plan_disabled.contains(id)) {
      return st.effects_plan_disabled.value(id);
    }

    const auto kind = AvailabilityKindForEffectId(id);
    if (kind == EffectAvailabilityKind::always) {
      return "Effect is unavailable.";
    }

    if (kind == EffectAvailabilityKind::gpu_utility) {
      if (!(maxineSupported || openCudaSupported)) return "GPU processing unavailable (no CUDA engine available).";
      return "GPU processing unavailable.";
    }

    if (enginePref == studiocast::video::effects::EffectsEnginePreference::open_cuda) {
      if (!st.open_cuda_present) {
        return "Open CUDA status not reported by daemon.";
      }
      if (!st.open_cuda_ok) {
        QStringList lines;
        lines << "Open CUDA unavailable.";
        if (st.open_cuda_installed_models.isEmpty()) {
          lines << "No usable Open CUDA model packs were found.";
        }
        if (!st.open_cuda_missing_models.isEmpty()) {
          lines << "";
          lines << "Missing/invalid model packs:";
          for (auto it = st.open_cuda_missing_models.begin(); it != st.open_cuda_missing_models.end(); ++it) {
            lines << ("• " + it.key() + ": " + it.value());
          }
        }
        if (!st.open_cuda_install_hints.isEmpty()) {
          lines << "";
          lines << st.open_cuda_install_hints;
        }
        lines << "";
        lines << "Run: studiocast-open install-hints";
        return lines.join("\n");
      }
      if (st.open_cuda_blocked_effects.contains(id)) {
        return st.open_cuda_blocked_effects.value(id);
      }
      return "Effect is unavailable.";
    }

    // Maxine (or auto_select with Maxine-specific reason).
    if (!maxineSupported) {
      if (!st.maxine_blocked_details.isEmpty()) return st.maxine_blocked_details.join("\n");
      if (!st.maxine_summary.isEmpty()) return st.maxine_summary;
      if (!st.maxine_blocked_reason.isEmpty()) return FormatMaxineReasonCode(st.maxine_blocked_reason);
      return "Maxine unavailable.";
    }

    if (st.maxine_missing_effects.contains(id)) {
      const auto reasons = st.maxine_missing_effects.value(id);
      if (!reasons.isEmpty()) {
        QStringList out;
        out.reserve(reasons.size());
        for (const auto& r : reasons) out.push_back(FormatMaxineReasonCode(r));
        return out.join("\n");
      }
      return "Effect is unavailable.";
    }

    if (st.maxine_available_effects.isEmpty() && st.maxine_missing_effects.isEmpty()) {
      return "Effect availability not reported by daemon.";
    }
    return "Effect is unavailable.";
  };

  auto setAvail = [&](QWidget* w, bool avail, const QString& tooltip) {
    if (!w) return;
    w->setEnabled(avail);
    w->setToolTip(avail ? QString() : tooltip);
  };

  // Virtual Background
  const QString vbMode = backgroundCombo_ ? backgroundCombo_->currentData().toString() : QString();
  const bool vbBlurAvail = effectAvailable(QStringLiteral("virtual_background.blur"));
  const bool vbRemoveAvail = effectAvailable(QStringLiteral("virtual_background.remove"));
  const bool vbReplaceAvail = effectAvailable(QStringLiteral("virtual_background.replace"));
  const bool afAvail = effectAvailable(QStringLiteral("auto_frame"));
  const bool vbAnyAvail = vbBlurAvail || vbRemoveAvail || vbReplaceAvail;
  const bool vbOn = (vbMode == "blur" || vbMode == "remove" || vbMode == "replace");
  const bool afOn = (vbMode == "auto_frame");
  if (backgroundCombo_) {
    // Allow switching back to "off" even when Maxine is unavailable / effect is missing.
    const bool allow = (vbAnyAvail || afAvail) || vbOn || afOn;
    const QString tipId = vbOn ? (QStringLiteral("virtual_background.") + vbMode)
                              : (afOn ? QStringLiteral("auto_frame") : QStringLiteral("virtual_background.blur"));
    setAvail(backgroundCombo_, allow, effectUnavailableTooltip(tipId));

    // Disable unavailable modes, but keep "off" selectable.
    auto* m = qobject_cast<QStandardItemModel*>(backgroundCombo_->model());
    if (m) {
      for (int i = 0; i < backgroundCombo_->count(); ++i) {
        const QString mode = backgroundCombo_->itemData(i).toString();
        bool itemEnabled = true;
        if (mode == "blur") itemEnabled = vbBlurAvail;
        else if (mode == "remove") itemEnabled = vbRemoveAvail;
        else if (mode == "replace") itemEnabled = vbReplaceAvail;
        else if (mode == "auto_frame") itemEnabled = afAvail;
        // "off" stays enabled.
        if (auto* item = m->item(i)) item->setEnabled(itemEnabled);
      }
    }
  }
  if (backgroundStrengthSpin_ && backgroundCombo_) {
    backgroundStrengthSpin_->setEnabled(vbMode == "blur" && vbBlurAvail);
  }
  if (backgroundRemoveColorEdit_ && backgroundCombo_) {
    backgroundRemoveColorEdit_->setEnabled(vbMode == "remove" && vbRemoveAvail);
  }
  if (backgroundReplaceImageEdit_ && backgroundCombo_) {
    const bool on = vbMode == "replace" && vbReplaceAvail;
    backgroundReplaceImageEdit_->setEnabled(on);
    if (browseReplaceImageBtn_) browseReplaceImageBtn_->setEnabled(on);
  }

  // Virtual Background model selection (Open CUDA-only).
  if (vbModelLabel_ && vbModelCombo_) {
    // Populate model list from daemon status (best-effort).
    if (daemonReachable_ && st.open_cuda_present) {
      bool hasTaskMeta = false;
      for (const auto& m : st.open_cuda_models) {
        if (!m.task.isEmpty()) {
          hasTaskMeta = true;
          break;
        }
      }

      QString sig;
      sig += QStringLiteral("matting|");
      sig += st.open_cuda_default_model_id;
      sig += QChar('|');

      for (const auto& m : st.open_cuda_models) {
        if (hasTaskMeta && m.task != QStringLiteral("matting")) continue;
        sig += m.id + QChar('\n');
        sig += m.display_name + QChar('\n');
        sig += m.task + QChar('\n');
      }

      if (sig != vbModelItemsSig_) {
        vbModelCombo_->blockSignals(true);
        vbModelCombo_->clear();
        vbModelCombo_->addItem(QStringLiteral("Default (auto)"), QString());

        for (const auto& m : st.open_cuda_models) {
          if (m.id.isEmpty()) continue;
          if (hasTaskMeta && m.task != QStringLiteral("matting")) continue;
          const QString label = m.display_name.isEmpty() ? m.id : (m.display_name + QStringLiteral("  [") + m.id + QChar(']'));
          vbModelCombo_->addItem(label, m.id);
        }

        vbModelItemsSig_ = sig;
        vbModelCombo_->blockSignals(false);
      }

      // Keep selection in sync with the canonical local model.
      const QString selectedId = QString::fromStdString(effects_.virtual_background.model_id);
      vbModelCombo_->blockSignals(true);

      // If config references a model that isn't installed, show it explicitly instead of silently
      // falling back to Default.
      if (vbModelCombo_->count() > 0 && vbModelCombo_->itemText(0).startsWith(QStringLiteral("Missing: "))) {
        vbModelCombo_->removeItem(0);
      }

      int idx = vbModelCombo_->findData(selectedId);
      if (!selectedId.isEmpty() && idx < 0) {
        const QString missingLabel = QStringLiteral("Missing: %1 (select a valid model)").arg(selectedId);
        vbModelCombo_->insertItem(0, missingLabel, selectedId);
        idx = 0;
      }

      vbModelCombo_->setCurrentIndex(idx >= 0 ? idx : 0);
      vbModelCombo_->blockSignals(false);
    }

    // Show only when VB is active and the daemon reports Open CUDA as the actual backend.
    QMap<QString, QString> backendById;
    if (!st.effects_backends.isEmpty()) {
      const auto parts = st.effects_backends.split(',', Qt::SkipEmptyParts);
      for (const auto& raw : parts) {
        const QString p = raw.trimmed();
        const qsizetype colon = p.indexOf(QChar(':'));
        if (colon <= 0) continue;
        const QString id = p.left(colon).trimmed();
        const QString backend = p.mid(colon + 1).trimmed();
        if (!id.isEmpty() && !backend.isEmpty()) backendById.insert(id, backend);
      }
    }

    QString vbEffectId;
    if (vbMode == QStringLiteral("blur")) vbEffectId = QStringLiteral("virtual_background.blur");
    else if (vbMode == QStringLiteral("remove")) vbEffectId = QStringLiteral("virtual_background.remove");
    else if (vbMode == QStringLiteral("replace")) vbEffectId = QStringLiteral("virtual_background.replace");

    const QString vbBackend = backendById.value(vbEffectId);
    const bool showModelRow = daemonReachable_ && (st.pipeline_running || st.pipeline_starting) && vbOn &&
                              vbBackend.compare(QStringLiteral("open_cuda"), Qt::CaseInsensitive) == 0;
    vbModelLabel_->setVisible(showModelRow);
    vbModelCombo_->setVisible(showModelRow);
  }

  // Open Video model selection for other effects (stored under
  // ~/.local/share/studiocast/models/open_video/<task>/...).
  {
    const auto reg = studiocast::open_video::ModelPackRegistry::ScanDefault();
    const auto models = reg.ListModels();

    auto update_open_video_model_combo = [&](const char* task,
                                             QLabel* label,
                                             QComboBox* combo,
                                             QString* items_sig,
                                             const std::string& selected_model_id,
                                             bool show_row) {
      if (!label || !combo || !items_sig) return;

      label->setVisible(show_row);
      combo->setVisible(show_row);

      std::vector<studiocast::open_video::ModelPack> packs;
      packs.reserve(models.size());
      for (const auto& m : models) {
        if (m.task == task) packs.push_back(m);
      }
      std::sort(packs.begin(), packs.end(), [](const auto& a, const auto& b) {
        const std::string& an = a.display_name.empty() ? a.id : a.display_name;
        const std::string& bn = b.display_name.empty() ? b.id : b.display_name;
        return an < bn;
      });

      std::string default_id = reg.DefaultModelIdForTask(task);
      if (default_id.empty() && !packs.empty()) default_id = packs.front().id;

      QString sig = QString::fromStdString(std::string(task) + "|" + default_id);
      for (const auto& p : packs) {
        sig += QString::fromStdString("|" + p.id + ":" + p.display_name);
      }

      if (sig != *items_sig) {
        QSignalBlocker b(combo);
        combo->clear();

        if (packs.empty()) {
          combo->addItem("<no models installed>", QString());
        } else {
          if (!default_id.empty()) {
            combo->addItem(QString("<auto: %1>").arg(QString::fromStdString(default_id)), QString());
          } else {
            combo->addItem("<auto>", QString());
          }

          for (const auto& p : packs) {
            QString text;
            if (!p.display_name.empty() && p.display_name != p.id) {
              text = QString("%1 (%2)").arg(QString::fromStdString(p.display_name), QString::fromStdString(p.id));
            } else {
              text = QString::fromStdString(p.id);
            }
            combo->addItem(text, QString::fromStdString(p.id));
          }
        }

        *items_sig = sig;
      }

      {
        QSignalBlocker b(combo);
        const QString want = QString::fromStdString(selected_model_id);
        if (want.isEmpty()) {
          combo->setCurrentIndex(0);
        } else {
          int idx = -1;
          for (int i = 0; i < combo->count(); ++i) {
            if (combo->itemData(i).toString() == want) {
              idx = i;
              break;
            }
          }

          if (idx >= 0) {
            combo->setCurrentIndex(idx);
          } else {
            combo->insertItem(1, QString("<missing: %1>").arg(want), want);
            combo->setCurrentIndex(1);
          }
        }
      }

      const bool has_models = !packs.empty();
      combo->setEnabled(daemonReachable_ && has_models);
      label->setEnabled(daemonReachable_);
      if (!has_models) {
        const QString tip = QString("No models installed for task '%1'.").arg(task);
        combo->setToolTip(tip);
        label->setToolTip(tip);
      } else {
        combo->setToolTip(QString());
        label->setToolTip(QString());
      }
    };

    update_open_video_model_combo("face_detection",
                                 autoFrameModelLabel_,
                                 autoFrameModelCombo_,
                                 &autoFrameModelItemsSig_,
                                 effects_.auto_frame.model_id,
                                 effects_.auto_frame.enabled);
    update_open_video_model_combo("eye_contact",
                                 eyeContactModelLabel_,
                                 eyeContactModelCombo_,
                                 &eyeContactModelItemsSig_,
                                 effects_.eye_contact.model_id,
                                 effects_.eye_contact.enabled);
    update_open_video_model_combo("video_denoise",
                                 denoiseModelLabel_,
                                 denoiseModelCombo_,
                                 &denoiseModelItemsSig_,
                                 effects_.video_noise_removal.model_id,
                                 effects_.video_noise_removal.enabled);
  }

  // Auto Frame
  const bool afAvailable = effectAvailable(QStringLiteral("auto_frame"));
  const bool afCheckOn = autoFrameCheck_ ? autoFrameCheck_->isChecked() : false;
  if (autoFrameCheck_) {
    const bool allow = afAvailable || afCheckOn;
    setAvail(autoFrameCheck_, allow, effectUnavailableTooltip(QStringLiteral("auto_frame")));
  }
  if (autoFrameZoomSlider_) setAvail(autoFrameZoomSlider_, afAvailable && afCheckOn, effectUnavailableTooltip(QStringLiteral("auto_frame")));

  // Eye Contact
  const bool ecAvailable = effectAvailable("eye_contact");
  const bool ecOn = eyeContactCheck_ ? eyeContactCheck_->isChecked() : false;
  if (eyeContactCheck_) {
    const bool allow = ecAvailable || ecOn;
    setAvail(eyeContactCheck_, allow, effectUnavailableTooltip("eye_contact"));
  }
  if (eyeContactStrengthSlider_) setAvail(eyeContactStrengthSlider_, ecAvailable && ecOn, effectUnavailableTooltip("eye_contact"));
  if (eyeContactLookAwayCheck_) setAvail(eyeContactLookAwayCheck_, ecAvailable && ecOn, effectUnavailableTooltip("eye_contact"));

  // Video Noise Removal
  const bool dnAvailable = effectAvailable("video_noise_removal");
  const bool dnOn = denoiseCheck_ ? denoiseCheck_->isChecked() : false;
  if (denoiseCheck_) {
    const bool allow = dnAvailable || dnOn;
    setAvail(denoiseCheck_, allow, effectUnavailableTooltip("video_noise_removal"));
  }
  if (denoiseStrengthSlider_) setAvail(denoiseStrengthSlider_, dnAvailable && dnOn, effectUnavailableTooltip("video_noise_removal"));

  // Virtual Key Light gating based on Maxine diagnostics.
  const bool vklAvailable = effectAvailable("virtual_key_light");
  const bool vklOn = virtualKeyLightCheck_ ? virtualKeyLightCheck_->isChecked() : false;

  if (virtualKeyLightCheck_) {
    const bool allow = vklAvailable || vklOn;
    setAvail(virtualKeyLightCheck_, allow, effectUnavailableTooltip("virtual_key_light"));
  }
  if (virtualKeyLightIntensitySpin_) setAvail(virtualKeyLightIntensitySpin_, vklAvailable && vklOn, effectUnavailableTooltip("virtual_key_light"));
  if (virtualKeyLightTempCombo_) setAvail(virtualKeyLightTempCombo_, vklAvailable && vklOn, effectUnavailableTooltip("virtual_key_light"));
  if (virtualKeyLightPanSpin_) setAvail(virtualKeyLightPanSpin_, vklAvailable && vklOn, effectUnavailableTooltip("virtual_key_light"));
  if (virtualKeyLightHdriEdit_) setAvail(virtualKeyLightHdriEdit_, vklAvailable && vklOn, effectUnavailableTooltip("virtual_key_light"));
  if (browseVirtualKeyLightHdriBtn_) setAvail(browseVirtualKeyLightHdriBtn_, vklAvailable && vklOn, effectUnavailableTooltip("virtual_key_light"));

  // Vignette (GPU utility).
  const bool vigAvailable = effectAvailable(QStringLiteral("vignette"));
  const bool vigOn = vignetteCheck_ ? vignetteCheck_->isChecked() : false;
  if (vignetteCheck_) {
    const bool allow = vigAvailable || vigOn;
    setAvail(vignetteCheck_, allow, effectUnavailableTooltip(QStringLiteral("vignette")));
  }
  if (vignetteIntensitySlider_) setAvail(vignetteIntensitySlider_, vigAvailable && vigOn, effectUnavailableTooltip(QStringLiteral("vignette")));
  if (vignetteCenterOnFaceCheck_) setAvail(vignetteCenterOnFaceCheck_, vigAvailable && vigOn, effectUnavailableTooltip(QStringLiteral("vignette")));

  startBtn_->setEnabled(daemonReachable_ && !enabled && outSelectable && !outputCombo_->currentData().toString().isEmpty());
  stopBtn_->setEnabled(daemonReachable_ && enabled);

  // Keep preview in sync with enabled state.
  if (enabled && daemonReachable_ && !previewCapture_.IsOpen()) {
    if (DebugGuiPreview()) {
      GuiPreviewDbg("UpdateUiEnabled: enabled=1 but preview not open -> StartPreview()");
    }
    StartPreview();
  }
  if (!enabled && previewCapture_.IsOpen()) {
    if (DebugGuiPreview()) {
      GuiPreviewDbg("UpdateUiEnabled: enabled=0 but preview open -> StopPreview()");
    }
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

  const auto fmtPixfmt = [](const QString& pixfmt) -> QString {
    if (pixfmt == QStringLiteral("RGB3")) return QStringLiteral("RGB24");
    if (pixfmt == QStringLiteral("BGR3")) return QStringLiteral("BGR24");
    return pixfmt;
  };

  const auto fmtFps = [](double fps) -> std::string {
    if (fps <= 0.0) return {};
    const double r = std::round(fps);
    if (std::fabs(fps - r) < 0.01) {
      return std::to_string(static_cast<int>(r));
    }
    QString s = QString::number(fps, 'f', 2);
    while (s.contains('.') && (s.endsWith('0') || s.endsWith('.'))) s.chop(1);
    return s.toStdString();
  };

  const auto fmtDims = [](int w, int h) -> std::string {
    return (QString::number(w) + QChar(0x00D7) + QString::number(h)).toStdString();
  };

  const auto fmtLine = [&](const DaemonVideoStatus::NegotiatedFormat& f, bool withPixfmtFirst) -> std::string {
    if (!f.present || f.width <= 0 || f.height <= 0) return "—";

    std::ostringstream line;
    const QString pix = fmtPixfmt(f.pixfmt);
    const std::string fpsStr = fmtFps(f.fps);

    if (withPixfmtFirst && !pix.isEmpty()) {
      line << pix.toStdString() << " ";
    }
    line << fmtDims(f.width, f.height);
    if (!fpsStr.empty()) {
      line << " @ " << fpsStr;
    }
    if (!withPixfmtFirst && !pix.isEmpty()) {
      line << " (" << pix.toStdString() << ")";
    }
    if (f.bytes_per_line > 0) {
      line << " (stride " << f.bytes_per_line << ")";
    }
    return line.str();
  };

  if (st.pipeline_running) {
    oss << "  Capture:    " << fmtLine(st.capture_format, /*withPixfmtFirst=*/true) << "\n";
    oss << "  Output:     " << fmtLine(st.output_format, /*withPixfmtFirst=*/false) << "\n";

    {
      std::ostringstream line;
      if (!st.scaling_backend_active.isEmpty()) {
        line << st.scaling_backend_active.toStdString();
      } else {
        line << "—";
      }

      const auto& from = st.scaling_from.present ? st.scaling_from : st.capture_format;
      const auto& to = st.scaling_to.present ? st.scaling_to : st.output_format;

      if (from.present && to.present && from.width > 0 && from.height > 0 && to.width > 0 && to.height > 0) {
        line << " (" << fmtDims(from.width, from.height) << " → " << fmtDims(to.width, to.height) << ")";
      }

      oss << "  Scaling:    " << line.str() << "\n";
    }
  } else {
    oss << "  Capture:    —\n";
    oss << "  Output:     —\n";
    oss << "  Scaling:    —\n";
  }

  oss << "  input:      " << st.input_device.toStdString() << "\n";
  oss << "  output:     " << st.output_device.toStdString() << "\n";
  oss << "  requested:  " << st.width << "x" << st.height << " @ " << st.fps << " fps\n";
  if (st.effects_valid) {
    oss << "  mirror:     " << (st.effects.mirror ? "on" : "off") << "\n";

    const bool autoFrame = st.effects.auto_frame.enabled;
    const std::string vbMode = autoFrame ? "auto_frame" : studiocast::video::effects::ToString(st.effects.virtual_background.mode);

    oss << "  background: " << vbMode;
    if (vbMode == "blur" || vbMode == "remove" || vbMode == "replace") {
      oss << " strength=" << st.effects.virtual_background.strength;
    }
    if ((vbMode == "remove" || vbMode == "replace") && !st.effects.virtual_background.remove_color.empty()) {
      oss << " color=" << st.effects.virtual_background.remove_color;
    }
    if (vbMode == "replace" && !st.effects.virtual_background.replace_path.empty()) {
      oss << " image=" << st.effects.virtual_background.replace_path;
    }
    oss << "\n";
  } else {
    oss << "  effects:    (failed to parse video_effects)\n";
  }

  if (!st.maxine_summary.isEmpty()) {
    oss << "  maxine:     " << st.maxine_summary.toStdString() << "\n";
    oss << "  vkl avail:  " << (st.virtual_key_light_available ? "yes" : "no") << "\n";
  }

  if (st.effects_valid) {
    const auto presetToStr = [](int p) -> const char* {
      switch (p) {
        case 1:
          return "warm";
        case 2:
          return "cool";
        default:
          return "neutral";
      }
    };
    oss << "  key light:  " << (st.effects.virtual_key_light.enabled ? "on" : "off")
        << " intensity=" << st.effects.virtual_key_light.intensity << "%"
        << " temp=" << presetToStr(st.effects.virtual_key_light.temperature_preset)
        << " pan=" << st.effects.virtual_key_light.direction_pan_degrees;
    if (!st.effects.virtual_key_light.hdri_path.empty()) {
      oss << " hdri=" << st.effects.virtual_key_light.hdri_path;
    }
    oss << "\n";
  }

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