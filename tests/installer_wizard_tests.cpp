#include <cstdlib>
#include <iostream>

#include <QApplication>
#include <QProgressBar>
#include <QStringList>

#include "installer_wizard.h"

namespace {

bool Expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << "\n";
    return false;
  }
  return true;
}

bool TestOpenBackendSetupArguments() {
  studiocast::installer::InstallerWizard wizard;
  wizard.setWorkflow(QStringLiteral("repair"));

  wizard.setOpenBackendsSetup(true);
  QStringList args = wizard.backendOptions(true);
  if (!Expect(
          args.contains(QStringLiteral("--open-backends")),
          "enabled Open Source backend setup should pass --open-backends") ||
      !Expect(!args.contains(QStringLiteral("--no-open-backends")),
              "enabled Open Source backend setup should not pass "
              "--no-open-backends")) {
    return false;
  }

  wizard.setOpenBackendsSetup(false);
  args = wizard.backendOptions(true);
  return Expect(args.contains(QStringLiteral("--no-open-backends")),
                "disabled Open Source backend setup should pass "
                "--no-open-backends") &&
	         Expect(!args.contains(QStringLiteral("--open-backends")),
	                "disabled Open Source backend setup should not pass "
	                "--open-backends");
}

bool TestReviewPageUsesFinishCommitButton() {
  studiocast::installer::InstallerWizard wizard;
  const QWizardPage *reviewPage =
      wizard.page(studiocast::installer::PageReview);
  const QWizardPage *progressPage =
      wizard.page(studiocast::installer::PageProgress);

  return Expect(reviewPage != nullptr, "review page should exist") &&
         Expect(reviewPage->isCommitPage(),
                "review page should use the commit button") &&
         Expect(progressPage != nullptr, "progress page should exist") &&
         Expect(!progressPage->isCommitPage(),
                "progress page should not use the commit button") &&
         Expect(wizard.buttonText(QWizard::CommitButton) ==
                    QStringLiteral("Finish"),
                "review page commit button should read Finish");
}

bool TestPreferenceProgressBars() {
  studiocast::installer::InstallerWizard wizard;
  constexpr int kFirstPage = studiocast::installer::PageIntro;
  constexpr int kLastPreferencePage = studiocast::installer::PageReview;
  constexpr int kMaximum = kLastPreferencePage - kFirstPage + 1;

  bool ok = true;
  for (int pageId = kFirstPage; pageId <= kLastPreferencePage; ++pageId) {
    const QWizardPage *page = wizard.page(pageId);
    ok = Expect(page != nullptr, "preference page should exist") && ok;
    if (!page) {
      continue;
    }

    const auto *bar = page->findChild<QProgressBar *>(
        QStringLiteral("scInstallerPreferenceProgress"));
    ok = Expect(bar != nullptr, "preference page should include progress bar") &&
         ok;
    if (!bar) {
      continue;
    }

    ok = Expect(bar->minimum() == 0, "progress bar should start at zero") &&
         ok;
    ok = Expect(bar->maximum() == kMaximum,
                "progress bar maximum should match preference page count") &&
         ok;
    ok = Expect(bar->value() == pageId - kFirstPage + 1,
                "progress bar value should match page position") &&
         ok;
    ok = Expect(
             bar->property("scRole").toString() ==
                 QStringLiteral("installerPreferenceProgress"),
             "progress bar should use installer style role") &&
         ok;
    ok = Expect(!bar->isTextVisible(), "progress bar text should be hidden") &&
         ok;
  }

  return ok;
}

} // namespace

int main(int argc, char **argv) {
  (void)::setenv("QT_QPA_PLATFORM", "offscreen", 0);
  QApplication app(argc, argv);

  bool ok = true;
  ok = TestOpenBackendSetupArguments() && ok;
  ok = TestReviewPageUsesFinishCommitButton() && ok;
  ok = TestPreferenceProgressBars() && ok;
  return ok ? 0 : 1;
}
