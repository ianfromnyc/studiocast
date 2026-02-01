#pragma once

#include <QWidget>
#include <string>
#include <vector>
#include <cstdint>

#include "core/video/camera_pipeline.h"

class QCheckBox;
class QLabel;
class QComboBox;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
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
        void OnPoll();

    private:
        void ShowError(const QString& title, const QString& details);
        void UpdateUiEnabled();
        void UpdateStatusText();

        QComboBox* inputCombo_ = nullptr;
        QComboBox* outputCombo_ = nullptr;

        QSpinBox* widthSpin_ = nullptr;
        QSpinBox* heightSpin_ = nullptr;
        QSpinBox* fpsSpin_ = nullptr;

        QCheckBox* mirrorCheck_ = nullptr;

        QPushButton* refreshBtn_ = nullptr;
        QPushButton* copyCmdBtn_ = nullptr;
        QPushButton* startBtn_ = nullptr;
        QPushButton* stopBtn_ = nullptr;
        QLabel* previewLabel_ = nullptr;

        QPlainTextEdit* statusText_ = nullptr;
        QTimer* pollTimer_ = nullptr;

        QString suggestedCmd_;
        std::string baseStatusText_;

        std::vector<std::uint8_t> previewRgb_;
        std::uint64_t previewSeq_ = 0;

        studiocast::video::CameraPipeline pipeline_;
    };

}  // namespace studiocast::gui
