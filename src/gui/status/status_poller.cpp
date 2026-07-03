#include "status_poller.h"

#include <QTimer>

#include "core/ipc/daemon_client.h"

namespace studiocast::gui {

StatusPoller::StatusPoller(QObject *parent) : QObject(parent) {
  timer_ = new QTimer(this);
  timer_->setTimerType(Qt::CoarseTimer);
  connect(timer_, &QTimer::timeout, this, &StatusPoller::Poll);
}

void StatusPoller::Start(int intervalMs) {
  timer_->start(intervalMs);
  Poll();
}

void StatusPoller::Stop() { timer_->stop(); }

void StatusPoller::Poll() {
  studiocast::ipc::DaemonCallResult res;
  studiocast::ipc::DaemonCallOptions options;
  options.connect_timeout_ms = 500;
  options.io_timeout_ms = 2500;

  std::string err;
  if (!studiocast::ipc::DaemonCall("GET_STATUS", &res, &err, options)) {
    snapshot_ =
        DaemonStatusSnapshot::Unreachable(QString::fromStdString(err));
    emit StatusChanged(snapshot_);
    return;
  }

  if (!res.ok) {
    snapshot_ = DaemonStatusSnapshot::Unreachable(
        QString::fromStdString(res.error_json.empty() ? "daemon_error"
                                                      : res.error_json));
    emit StatusChanged(snapshot_);
    return;
  }

  snapshot_ = DaemonStatusSnapshot::FromJson(QString::fromStdString(res.json));
  emit StatusChanged(snapshot_);
}

} // namespace studiocast::gui
