#pragma once

// Select / Create Package: the data packages of a profile's institution, as
// EarthBank lists them, to choose the one uploads go to; or a new one, made
// under the name typed. The list is asked for when the dialog opens and comes
// in on the worker's thread, so the dialog is there at once and says that it
// is waiting. OK gives the chosen package's id.
//
// Every package of the institution is listed, not only those the account may
// write to: EarthBank does not say which those are (session.hpp). One the
// account made itself always can be written to.

#include <cstdint>
#include <optional>
#include <vector>

#include <QDialog>
#include <QString>

#include "pychron/earthbank/profiles.hpp"
#include "pychron/earthbank/session.hpp"

class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

namespace pychron::ui {

class EarthbankWorker;

class EarthbankPackageDialog : public QDialog {
  Q_OBJECT

 public:
  // `worker` must outlive the dialog. `password`: as the worker takes it
  // (empty: the stored one).
  EarthbankPackageDialog(EarthbankWorker& worker, earthbank::Profile profile, QString password,
                         QWidget* parent = nullptr);

  // The package of the selected row; nothing when none is selected.
  std::optional<std::int64_t> selected_id() const;

  // For tests.
  QTableWidget* packages() const noexcept { return packages_; }  // name, id, state
  QLineEdit* filter() const noexcept { return filter_; }
  QLineEdit* name() const noexcept { return name_; }
  QPushButton* create_button() const noexcept { return create_; }
  QLabel* status() const noexcept { return status_; }
  QDialogButtonBox* buttons() const noexcept { return buttons_; }

 private:
  int add_row(const earthbank::DataPackage& package);
  void apply_filter();
  void update_buttons();
  void say(const QString& what, bool wrong);

  EarthbankWorker& worker_;
  earthbank::Profile profile_;
  QString password_;
  bool creating_ = false;
  QLineEdit* filter_;
  QTableWidget* packages_;
  QLineEdit* name_;
  QPushButton* create_;
  QLabel* status_;
  QDialogButtonBox* buttons_;
};

}  // namespace pychron::ui
