#pragma once

#include <QObject>

#include "daemon_status_snapshot.h"

class QTimer;

namespace studiocast::gui {

class StatusPoller final : public QObject {
  Q_OBJECT

public:
  explicit StatusPoller(QObject *parent = nullptr);

  void Start(int intervalMs = 2000);
  void Stop();
  void PollNow();
  const DaemonStatusSnapshot &snapshot() const { return snapshot_; }

signals:
  void StatusChanged(const studiocast::gui::DaemonStatusSnapshot &snapshot);

private slots:
  void Poll();

private:
  QTimer *timer_ = nullptr;
  DaemonStatusSnapshot snapshot_;
};

} // namespace studiocast::gui
