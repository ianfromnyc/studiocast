#pragma once

namespace studiocast::gui {

class PendingDaemonWriteGuard {
public:
  void MarkPending() { pending_ = true; }
  void MarkWriteAccepted() { pending_ = false; }
  void MarkWriteRejected() { pending_ = false; }

  bool HasPendingWrite() const { return pending_; }
  bool ShouldApplyRoutineStatus() const { return !pending_; }

private:
  bool pending_ = false;
};

} // namespace studiocast::gui
