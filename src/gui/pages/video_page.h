#pragma once

#include <QWidget>
#include <string>

#include "core/video/feed.h"

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
        void OnStartFeed();
        void OnStopFeed();
        void OnPoll();

    private:
        void ShowError(const QString& title, const QString& details);
        void UpdateUiEnabled();
        void UpdateStatusText();

        QComboBox* deviceCombo_ = nullptr;
        QSpinBox* widthSpin_ = nullptr;
        QSpinBox* heightSpin_ = nullptr;
        QSpinBox* fpsSpin_ = nullptr;
        QComboBox* formatCombo_ = nullptr;

        QPushButton* refreshBtn_ = nullptr;
        QPushButton* copyCmdBtn_ = nullptr;
        QPushButton* startBtn_ = nullptr;
        QPushButton* stopBtn_ = nullptr;

        QPlainTextEdit* statusText_ = nullptr;
        QTimer* pollTimer_ = nullptr;

        QString suggestedCmd_;
        std::string baseStatusText_;

        studiocast::video::VideoFeed feed_;
    };

}  // namespace studiocast::gui
