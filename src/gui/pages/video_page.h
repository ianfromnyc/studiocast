#pragma once

#include <QWidget>
#include <string>
#include <vector>

#include "core/ipc/daemon_client.h"
#include "core/video/effects/broadcast_effects.h"
#include "core/video/v4l2_capture.h"

class QCheckBox;
class QComboBox;
class QSlider;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QLabel;
class QTimer;
class QLineEdit;

namespace studiocast::gui {
class VideoPreviewWidget;
}  // namespace studiocast::gui

namespace studiocast::gui {

    class VideoPage final : public QWidget {
        Q_OBJECT
       public:
        explicit VideoPage(QWidget* parent = nullptr);
        ~VideoPage() override;

    private slots:
     void Refresh();
        void CopySuggestedCommand();
        void OnStart();
        void OnStop();
        void OnEnginePreferenceChanged(int index);
        void OnMirrorToggled(bool checked);
        void OnBackgroundChanged(int index);
        void OnBackgroundStrengthChanged(int value);
        void OnBackgroundRemoveColorChanged();
        void OnBackgroundReplaceImageChanged();
        void OnBrowseReplaceImage();

        void OnAutoFrameToggled(bool checked);
        void OnAutoFrameZoomChanged(int value);

        void OnEyeContactToggled(bool checked);
        void OnEyeContactStrengthChanged(int value);
        void OnEyeContactLookAwayToggled(bool checked);

        void OnDenoiseToggled(bool checked);
        void OnDenoiseStrengthChanged(int value);

        void OnOpenInstallHints();

        void OnVirtualKeyLightToggled(bool checked);
        void OnVirtualKeyLightIntensityChanged(int value);
        void OnVirtualKeyLightTemperatureChanged(int index);
        void OnVirtualKeyLightPanChanged(int value);
        void OnVirtualKeyLightHdriChanged();
        void OnBrowseVirtualKeyLightHdri();

        void OnVignetteToggled(bool checked);
        void OnVignetteIntensityChanged(int value);
        void OnVignetteCenterOnFaceToggled(bool checked);

        void OnPoll();

    private:
        void ShowError(const QString& title, const QString& details);
        void UpdateUiEnabled();
        void UpdateStatusText();
        bool SyncFromDaemonConfig();

        bool SendDaemonVideoConfig();
        bool SendDaemonVideoEffects();
        bool SendDaemonEnabled(bool enabled);

        void StartPreview();
        void StopPreview();
        void OnPreviewTick();

        QComboBox* inputCombo_ = nullptr;
        QComboBox* outputCombo_ = nullptr;

        QSpinBox* widthSpin_ = nullptr;
        QSpinBox* heightSpin_ = nullptr;
        QSpinBox* fpsSpin_ = nullptr;

        QComboBox* engineCombo_ = nullptr;

        QCheckBox* mirrorCheck_ = nullptr;

        QComboBox* backgroundCombo_ = nullptr;
        QSpinBox* backgroundStrengthSpin_ = nullptr;

        QLineEdit* backgroundRemoveColorEdit_ = nullptr;   // #RRGGBB
        QLineEdit* backgroundReplaceImageEdit_ = nullptr;  // path (PPM/P6 for now)
        QPushButton* browseReplaceImageBtn_ = nullptr;

        // Auto Frame (Maxine AR)
        QCheckBox* autoFrameCheck_ = nullptr;
        QSlider* autoFrameZoomSlider_ = nullptr;
        QLabel* autoFrameZoomValue_ = nullptr;

        // Eye Contact (Maxine AR)
        QCheckBox* eyeContactCheck_ = nullptr;
        QSlider* eyeContactStrengthSlider_ = nullptr;
        QLabel* eyeContactStrengthValue_ = nullptr;
        QCheckBox* eyeContactLookAwayCheck_ = nullptr;

        // Video Noise Removal (Maxine VFX)
        QCheckBox* denoiseCheck_ = nullptr;
        QSlider* denoiseStrengthSlider_ = nullptr;
        QLabel* denoiseStrengthValue_ = nullptr;

        // Virtual Key Light (Maxine relighting)
        QCheckBox* virtualKeyLightCheck_ = nullptr;
        QSpinBox* virtualKeyLightIntensitySpin_ = nullptr;  // 0..100
        QComboBox* virtualKeyLightTempCombo_ = nullptr;     // neutral|warm|cool
        QSpinBox* virtualKeyLightPanSpin_ = nullptr;        // -180..180
        QLineEdit* virtualKeyLightHdriEdit_ = nullptr;      // path override
        QPushButton* browseVirtualKeyLightHdriBtn_ = nullptr;

        // Vignette (GPU post-process)
        QCheckBox* vignetteCheck_ = nullptr;
        QSlider* vignetteIntensitySlider_ = nullptr;         // 0..100
        QLabel* vignetteIntensityValue_ = nullptr;
        QCheckBox* vignetteCenterOnFaceCheck_ = nullptr;

        QLabel* effectEngineValue_ = nullptr;
        QLabel* maxineBanner_ = nullptr;

        QPushButton* openInstallHintsBtn_ = nullptr;
        QPlainTextEdit* diagnosticsText_ = nullptr;

        QPushButton* refreshBtn_ = nullptr;
        QPushButton* copyCmdBtn_ = nullptr;
        QPushButton* startBtn_ = nullptr;
        QPushButton* stopBtn_ = nullptr;

        VideoPreviewWidget* preview_ = nullptr;
        QTimer* previewTimer_ = nullptr;

        QPlainTextEdit* statusText_ = nullptr;
        QTimer* pollTimer_ = nullptr;

        QString suggestedCmd_;
        std::string baseStatusText_;

        bool daemonReachable_ = false;
        std::string daemonLastStatusJson_;

        // Canonical local effects model (Broadcast schema). This is the single
        // source of truth for what the GUI intends to apply.
        studiocast::video::effects::BroadcastCameraEffects effects_{};

        // Preview is implemented by opening the virtual camera (output device)
        // as a consumer and rendering frames inside the GUI.
        studiocast::video::V4l2Capture previewCapture_;
        std::vector<uint8_t> previewRgb_;
        int previewW_ = 0;
        int previewH_ = 0;
        int previewBpl_ = 0;
    };

}  // namespace studiocast::gui
