#include "status_poller.h"

#include <QThread>
#include <QTimer>

#include "core/ipc/daemon_client.h"

namespace studiocast::gui {

namespace detail {

void DiagnosticsRefreshWorker::Run() {
  studiocast::ipc::DaemonCallResult res;
  studiocast::ipc::DaemonCallOptions options;
  options.connect_timeout_ms = 500;
  options.io_timeout_ms = 30000;

  std::string err;
  if (!studiocast::ipc::DaemonCall("REFRESH_DIAGNOSTICS", &res, &err,
                                   options)) {
    emit Finished(false, QString::fromStdString(err));
    return;
  }

  if (!res.ok) {
    emit Finished(
        false,
        QString::fromStdString(res.error_json.empty() ? "daemon_error"
                                                      : res.error_json));
    return;
  }

  emit Finished(true, QString());
}

} // namespace detail

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

void StatusPoller::PollNow() { Poll(); }

void StatusPoller::RefreshDiagnosticsNow() {
  if (diagnosticsRefreshInFlight_)
    return;

  diagnosticsRefreshInFlight_ = true;
  auto *thread = new QThread();
  auto *worker = new detail::DiagnosticsRefreshWorker();
  worker->moveToThread(thread);

  connect(thread, &QThread::started, worker,
          &detail::DiagnosticsRefreshWorker::Run);
  connect(worker, &detail::DiagnosticsRefreshWorker::Finished, thread,
          &QThread::quit);
  connect(worker, &detail::DiagnosticsRefreshWorker::Finished, worker,
          &QObject::deleteLater);
  connect(thread, &QThread::finished, thread, &QObject::deleteLater);
  connect(worker, &detail::DiagnosticsRefreshWorker::Finished, this,
          [this](bool ok, const QString &error) {
            diagnosticsRefreshInFlight_ = false;
            if (ok)
              Poll();
            emit DiagnosticsRefreshFinished(ok, error);
          });

  thread->start();
}

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
