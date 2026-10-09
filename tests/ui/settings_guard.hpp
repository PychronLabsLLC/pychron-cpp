#pragma once

// A window given no QSettings falls back to the application's own, a
// default-constructed QSettings(). In a test that is the developer's real
// preferences (on macOS ~/Library/Preferences/com.trolltech.unknown-organization.
// <test executable>.plist). A suite holds one of these for as long as it runs:
// the application's settings are then a file in a temporary directory, and
// cleanupTestCase requires that nothing was written there:
//
//     pychron::ui::test::ApplicationSettingsGuard app_settings_;
//     ...
//     void cleanupTestCase() { QCOMPARE(app_settings_.keys(), QStringList()); }
//
// Constructed after the QApplication (a member of the test object is).

#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

namespace pychron::ui::test {

class ApplicationSettingsGuard {
 public:
  ApplicationSettingsGuard() {
    // The native format's place cannot be moved; the ini format's can.
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir_.filePath(QStringLiteral("user")));
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, dir_.filePath(QStringLiteral("system")));
  }

  // What has been written to the application's settings: nothing, in a suite
  // that gives every window its own.
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static): reads where this object sent them
  [[nodiscard]] QStringList keys() const { return QSettings().allKeys(); }

 private:
  QTemporaryDir dir_;
};

}  // namespace pychron::ui::test
