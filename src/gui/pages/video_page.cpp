#include "video_page.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QVBoxLayout>

namespace studiocast::gui {

VideoPage::VideoPage(QWidget* parent) : QWidget(parent) {
  auto* root = new QVBoxLayout(this);
  root->setSpacing(12);

  auto* title = new QLabel("Camera", this);
  title->setStyleSheet("font-size: 20px; font-weight: 600;");
  root->addWidget(title);

  auto* topRow = new QHBoxLayout();

  // Left side: controls
  auto* controlsCol = new QVBoxLayout();

  auto* inputBox = new QGroupBox("Input", this);
  auto* inputLayout = new QHBoxLayout(inputBox);
  inputLayout->addWidget(new QLabel("Device:", inputBox));

  auto* cameras = new QComboBox(inputBox);
  cameras->addItem("Default webcam (placeholder)");
  cameras->addItem("External cam (placeholder)");
  inputLayout->addWidget(cameras, 1);
  controlsCol->addWidget(inputBox);

  auto* effectsBox = new QGroupBox("Effects (placeholders)", this);
  auto* effectsLayout = new QVBoxLayout(effectsBox);

  effectsLayout->addWidget(new QCheckBox("Virtual Background", effectsBox));
  effectsLayout->addWidget(new QCheckBox("Background Blur", effectsBox));
  effectsLayout->addWidget(new QCheckBox("Auto Frame", effectsBox));
  effectsLayout->addWidget(new QCheckBox("Eye Contact", effectsBox));
  effectsLayout->addWidget(new QCheckBox("Video Noise Removal", effectsBox));
  effectsLayout->addWidget(new QCheckBox("Virtual Key Light", effectsBox));

  auto* strengthRow = new QHBoxLayout();
  strengthRow->addWidget(new QLabel("Strength:", effectsBox));
  auto* strength = new QSlider(Qt::Horizontal, effectsBox);
  strength->setRange(0, 100);
  strength->setValue(60);
  strengthRow->addWidget(strength, 1);
  effectsLayout->addLayout(strengthRow);

  controlsCol->addWidget(effectsBox);

  auto* outputBox = new QGroupBox("Output", this);
  auto* outputLayout = new QVBoxLayout(outputBox);
  outputLayout->addWidget(new QLabel(
      "Phase 0: virtual camera not implemented.\n"
      "Later: v4l2loopback device \u201cStudioCast Camera\u201d.",
      outputBox));
  controlsCol->addWidget(outputBox);
  controlsCol->addStretch(1);

  // Right side: preview placeholder
  auto* previewBox = new QGroupBox("Preview", this);
  auto* previewLayout = new QVBoxLayout(previewBox);
  auto* preview = new QLabel("Preview will appear here (Phase 0 placeholder)", previewBox);
  preview->setMinimumSize(480, 360);
  preview->setAlignment(Qt::AlignCenter);
  preview->setStyleSheet("border: 1px solid rgba(255,255,255,0.2);");
  previewLayout->addWidget(preview, 1);

  topRow->addLayout(controlsCol, 1);
  topRow->addWidget(previewBox, 1);

  root->addLayout(topRow, 1);
}

}  // namespace studiocast::gui
