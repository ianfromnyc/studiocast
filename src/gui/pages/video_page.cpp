#include "video_page.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/video/v4l2loopback.h"

namespace studiocast::gui {

    VideoPage::VideoPage(QWidget* parent) : QWidget(parent) {
        auto* root = new QVBoxLayout(this);
        root->setSpacing(12);

        auto* title = new QLabel("Camera", this);
        title->setStyleSheet("font-size: 20px; font-weight: 600;");
        root->addWidget(title);

        auto* box = new QGroupBox("Virtual Camera (v4l2loopback)", this);
        auto* boxLayout = new QVBoxLayout(box);

        auto* btnRow = new QHBoxLayout();
        refreshBtn_ = new QPushButton("Refresh", box);
        copyCmdBtn_ = new QPushButton("Copy suggested command", box);

        btnRow->addWidget(refreshBtn_);
        btnRow->addWidget(copyCmdBtn_);
        btnRow->addStretch(1);
        boxLayout->addLayout(btnRow);

        statusText_ = new QPlainTextEdit(box);
        statusText_->setReadOnly(true);
        statusText_->setMinimumHeight(260);
        boxLayout->addWidget(statusText_, 1);

        root->addWidget(box);
        root->addStretch(1);

        connect(refreshBtn_, &QPushButton::clicked, this, &VideoPage::Refresh);
        connect(copyCmdBtn_, &QPushButton::clicked, this, &VideoPage::CopySuggestedCommand);

        Refresh();
    }

    void VideoPage::Refresh() {
        const auto rep = studiocast::video::ProbeLoopback();
        statusText_->setPlainText(QString::fromStdString(rep.ToText()));

        suggestedCmd_ = QString::fromStdString(rep.suggested_modprobe_cmd);
        copyCmdBtn_->setEnabled(!suggestedCmd_.isEmpty());
    }

    void VideoPage::CopySuggestedCommand() {
        if (suggestedCmd_.isEmpty()) return;
        if (auto* cb = QGuiApplication::clipboard()) {
            cb->setText(suggestedCmd_);
        }
    }

}  // namespace studiocast::gui
