#include "audio_page.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QVBoxLayout>

namespace studiocast::gui {

AudioPage::AudioPage(QWidget* parent) : QWidget(parent) {
  auto* root = new QVBoxLayout(this);
  root->setSpacing(12);

  auto* title = new QLabel("Microphone", this);
  title->setStyleSheet("font-size: 20px; font-weight: 600;");
  root->addWidget(title);

  auto* inputBox = new QGroupBox("Input", this);
  auto* inputLayout = new QHBoxLayout(inputBox);
  inputLayout->addWidget(new QLabel("Device:", inputBox));

  auto* inputDevices = new QComboBox(inputBox);
  inputDevices->addItem("Default (placeholder)");
  inputDevices->addItem("USB Mic (placeholder)");
  inputLayout->addWidget(inputDevices, 1);

  root->addWidget(inputBox);

  auto* effectsBox = new QGroupBox("Effects (placeholders)", this);
  auto* effectsLayout = new QVBoxLayout(effectsBox);

  effectsLayout->addWidget(new QCheckBox("Noise Removal", effectsBox));
  effectsLayout->addWidget(new QCheckBox("Echo Removal", effectsBox));
  effectsLayout->addWidget(new QCheckBox("Studio Voice", effectsBox));

  auto* strengthRow = new QHBoxLayout();
  strengthRow->addWidget(new QLabel("Strength:", effectsBox));
  auto* strength = new QSlider(Qt::Horizontal, effectsBox);
  strength->setRange(0, 100);
  strength->setValue(60);
  strengthRow->addWidget(strength, 1);
  effectsLayout->addLayout(strengthRow);

  root->addWidget(effectsBox);

  auto* outputBox = new QGroupBox("Output", this);
  auto* outputLayout = new QVBoxLayout(outputBox);
  outputLayout->addWidget(new QLabel(
      "Phase 0: virtual microphone not implemented.\n"
      "Later: PipeWire node that exposes \u201cStudioCast Microphone\u201d.",
      outputBox));

  root->addWidget(outputBox);
  root->addStretch(1);
}

}  // namespace studiocast::gui
