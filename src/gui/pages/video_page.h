#pragma once

#include <QWidget>

class QPlainTextEdit;
class QPushButton;

namespace studiocast::gui {

    class VideoPage final : public QWidget {
        Q_OBJECT
       public:
        explicit VideoPage(QWidget* parent = nullptr);

    private slots:
     void Refresh();
        void CopySuggestedCommand();

    private:
        QPlainTextEdit* statusText_ = nullptr;
        QPushButton* refreshBtn_ = nullptr;
        QPushButton* copyCmdBtn_ = nullptr;

        QString suggestedCmd_;
    };

}  // namespace studiocast::gui
