// ErrorReport: an error shown to the user is in the log first, on ui.<area> at
// error, and the box still comes up as before.

#include <algorithm>
#include <string>
#include <vector>

#include <QApplication>
#include <QMessageBox>
#include <QTimer>
#include <QtTest/QtTest>

#include "error_report.hpp"
#include "log_capture.hpp"

namespace {

bool has_line(const std::vector<std::string>& lines, const std::string& wanted) {
  return std::ranges::find(lines, wanted) != lines.end();
}

std::string joined(const std::vector<std::string>& lines) {
  std::string all;
  for (const auto& l : lines) all.append(l).push_back('\n');
  return all;
}

}  // namespace

class TestErrorReport : public QObject {
  Q_OBJECT

 private slots:
  void errorIsLoggedThenShown() {
    const pychron::testing::LogCapture capture;
    QVERIFY(capture.ok());
    QString shown;
    std::vector<std::string> logged_before_box;
    QTimer::singleShot(0, this, [&] {
      auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
      if (box == nullptr) return;
      shown = box->text();
      logged_before_box = capture.lines();
      box->close();
    });
    pychron::ui::show_error(nullptr, "experiment", QStringLiteral("Save"), QStringLiteral("cannot write q.txt"));
    QCOMPARE(shown, QStringLiteral("cannot write q.txt"));
    QVERIFY2(has_line(logged_before_box, "[error] ui.experiment: Save: cannot write q.txt"),
             joined(logged_before_box).c_str());

    QTimer::singleShot(0, this, [] {
      if (QWidget* box = QApplication::activeModalWidget()) box->close();
    });
    pychron::ui::show_warning(nullptr, "fit", QStringLiteral("Flux"), QStringLiteral("no level"));
    QVERIFY2(has_line(capture.lines(), "[error] ui.fit: Flux: no level"), joined(capture.lines()).c_str());
  }

  void errorObjectHasKindAndContext() {
    const pychron::testing::LogCapture capture;
    QVERIFY(capture.ok());
    QString shown;
    QTimer::singleShot(0, this, [&] {
      auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
      if (box == nullptr) return;
      shown = box->text();
      box->close();
    });
    const pychron::Error error{pychron::ErrorKind::Timeout, "no reply", "ig1"};
    pychron::ui::show_error(nullptr, "laser", QStringLiteral("Fire"), error);
    QCOMPARE(shown, QStringLiteral("no reply"));
    QVERIFY2(has_line(capture.lines(), "[error] ui.laser: Fire: timeout: no reply (ig1)"),
             joined(capture.lines()).c_str());
  }
};

QTEST_MAIN(TestErrorReport)
#include "test_error_report.moc"
