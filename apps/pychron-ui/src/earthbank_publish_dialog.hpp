#pragma once

// Data browser, Publish to EarthBank: a dialog around EarthbankPublishPanel
// (earthbank_publish_panel.hpp) with the Run button the panel leaves to its
// host. The Data window uses it until PublishDialog takes its place.

#include <functional>
#include <memory>
#include <vector>

#include <QDialog>
#include <QSettings>

#include "earthbank_publish_panel.hpp"

class QPushButton;

namespace pychron::ui {

class EarthbankPublishDialog : public QDialog {
  Q_OBJECT

 public:
  using SettingsFactory = EarthbankPublishPanel::SettingsFactory;

  EarthbankPublishDialog(std::vector<processing::AnalysisPtr> analyses, EarthbankSetup setup, SettingsFactory settings,
                         QWidget* parent = nullptr);

  EarthbankPublishPanel* panel() const noexcept { return panel_; }
  QPushButton* run_button() const noexcept { return run_; }

  // The rows are in, nothing is running and no request is out.
  bool ready() const { return panel_->ready(); }
  // A run (a check, an export, an upload) is under way.
  bool busy() const noexcept { return panel_->busy(); }

 private:
  EarthbankPublishPanel* panel_;
  QPushButton* run_;
};

}  // namespace pychron::ui
