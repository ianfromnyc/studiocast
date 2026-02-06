#pragma once

#include <QWidget>
#include <vector>
#include "core/audio/pulse/pactl.h"

class QComboBox;
class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSlider;
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

        void OnAiNoiseToggled(bool checked);
        void OnAiEchoToggled(bool checked);
        void OnAiStudioVoiceToggled(bool checked);
        void OnAiStrengthChanged(int v);

        void OnAiSpeakerNoiseToggled(bool checked);
        void OnAiSpeakerStrengthChanged(int v);

        void OnAiStart();
        void OnAiStop();

        void OnCreateVirtualMic();

        void OnDestroyVirtualMic();

        void OnStartLoopback();

        void OnStopLoopback();

        void OnSourceChanged(int index);

        void OnEnableVirtualSpeakers();

        void OnStopSpeakersRouting();

        void OnDestroyVirtualSpeakers();

    private:
        void ShowError(const QString &title, const QString &details);

        void RefreshDaemonAudioStatus();
        void PushDaemonAudioConfig();
        void PushDaemonSourceSelection();
        void SetAiControlsEnabled(bool enabled, const QString& reason);

        void UpdateMicInterlocks();

        QComboBox *sourceCombo_ = nullptr;
        QPushButton *refreshSourcesBtn_ = nullptr;

        QComboBox *portCombo_ = nullptr;
        std::vector<studiocast::audio::pulse::PactlSourceInfo> cachedSources_;


        QSpinBox *latencySpin_ = nullptr;

        // Daemon-driven AFX controls (MVP).
        QLabel* aiBanner_ = nullptr;
        QCheckBox* noiseRemovalCb_ = nullptr;
        QCheckBox* echoRemovalCb_ = nullptr;
        QCheckBox* studioVoiceCb_ = nullptr;
        QSlider* strengthSlider_ = nullptr;
        QLabel* strengthValueLabel_ = nullptr;

        QCheckBox* speakerNoiseRemovalCb_ = nullptr;
        QSlider* speakerStrengthSlider_ = nullptr;
        QLabel* speakerStrengthValueLabel_ = nullptr;

        QPushButton* aiStartBtn_ = nullptr;
        QPushButton* aiStopBtn_ = nullptr;
        QPushButton* aiRefreshBtn_ = nullptr;

        bool updatingAiUi_ = false;
        bool daemonAiSupported_ = false;
        QString daemonAiDisableReason_;
        QString daemonStatusText_;

        QPushButton *createBtn_ = nullptr;
        QPushButton *destroyBtn_ = nullptr;
        QPushButton *startBtn_ = nullptr;
        QPushButton *stopBtn_ = nullptr;

        QPushButton *enableSpeakersBtn_ = nullptr;
        QPushButton *stopSpeakersBtn_ = nullptr;
        QPushButton *destroySpeakersBtn_ = nullptr;

        QPlainTextEdit *statusText_ = nullptr;
        QPushButton *refreshStatusBtn_ = nullptr;

        QTimer *pollTimer_ = nullptr;
    };
} // namespace studiocast::gui
