#pragma once

#include <QWidget>
#include <vector>
#include "core/audio/pulse/pactl.h"

class QComboBox;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTimer;

namespace studiocast::gui {
    class AudioPage final : public QWidget {
        Q_OBJECT

    public:
        explicit AudioPage(QWidget *parent = nullptr);

    private slots:
        void RefreshSources();

        void RefreshStatus();

        void OnCreateVirtualMic();

        void OnDestroyVirtualMic();

        void OnStartLoopback();

        void OnStopLoopback();

        void OnSourceChanged(int index);

    private:
        void ShowError(const QString &title, const QString &details);

        QComboBox *sourceCombo_ = nullptr;
        QPushButton *refreshSourcesBtn_ = nullptr;

        QComboBox *portCombo_ = nullptr;
        std::vector<studiocast::audio::pulse::PactlSourceInfo> cachedSources_;


        QSpinBox *latencySpin_ = nullptr;

        QPushButton *createBtn_ = nullptr;
        QPushButton *destroyBtn_ = nullptr;
        QPushButton *startBtn_ = nullptr;
        QPushButton *stopBtn_ = nullptr;

        QPlainTextEdit *statusText_ = nullptr;
        QPushButton *refreshStatusBtn_ = nullptr;

        QTimer *pollTimer_ = nullptr;
    };
} // namespace studiocast::gui
