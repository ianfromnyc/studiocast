#pragma once

#include <QWidget>
#include <string>
#include <vector>

#include "core/ipc/daemon_client.h"
#include "core/video/v4l2_capture.h"

class QCheckBox;
class QComboBox;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QLabel;
class QTimer;

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
        void OnMirrorToggled(bool checked);
        void OnBackgroundChanged(int index);
        void OnBackgroundBackendChanged(int index);
        void OnBackgroundStrengthChanged(int value);
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

        QCheckBox* mirrorCheck_ = nullptr;

        QComboBox* backgroundCombo_ = nullptr;
        QComboBox* backgroundBackendCombo_ = nullptr;
        QSpinBox* backgroundStrengthSpin_ = nullptr;

        QPushButton* refreshBtn_ = nullptr;
        QPushButton* copyCmdBtn_ = nullptr;
        QPushButton* startBtn_ = nullptr;
        QPushButton* stopBtn_ = nullptr;

        QLabel* previewLabel_ = nullptr;
        QTimer* previewTimer_ = nullptr;

        QPlainTextEdit* statusText_ = nullptr;
        QTimer* pollTimer_ = nullptr;

        QString suggestedCmd_;
        std::string baseStatusText_;

        bool daemonReachable_ = false;
        std::string daemonLastStatusJson_;

        // Preview is implemented by opening the virtual camera (output device)
        // as a consumer and rendering frames inside the GUI.
        studiocast::video::V4l2Capture previewCapture_;
        std::vector<uint8_t> previewRgb_;
        int previewW_ = 0;
        int previewH_ = 0;
        int previewBpl_ = 0;
    };

}  // namespace studiocast::gui
