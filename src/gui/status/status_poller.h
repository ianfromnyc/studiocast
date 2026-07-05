#pragma once

#include <QObject>
#include <QString>

#include "daemon_status_snapshot.h"

class QTimer;

namespace studiocast::gui {

namespace detail {
class DiagnosticsRefreshWorker final : public QObject {
  Q_OBJECT

public slots:
  void Run();

signals:
  void Finished(bool ok, const QString &error);
};
} // namespace detail

class StatusPoller final : public QObject {
  Q_OBJECT

public:
  explicit StatusPoller(QObject *parent = nullptr);

  void Start(int intervalMs = 2000);
  void Stop();
  void PollNow();
  void RefreshDiagnosticsNow();
  const DaemonStatusSnapshot &snapshot() const { return snapshot_; }

signals:
  void StatusChanged(const studiocast::gui::DaemonStatusSnapshot &snapshot);
  void DiagnosticsRefreshFinished(bool ok, const QString &error);

private slots:
  void Poll();

private:
  QTimer *timer_ = nullptr;
  DaemonStatusSnapshot snapshot_;
  bool diagnosticsRefreshInFlight_ = false;
};

} // namespace studiocast::gui
