#pragma once

// The main window of a data-reduction install: the data browser over the
// install's database (or its records folder), with recall and figure windows,
// and no instrument.

#include <functional>

#include <QMainWindow>

#include "data_workspace.hpp"

class QAction;

namespace pychron::ui {

class DataMainWindow : public QMainWindow {
  Q_OBJECT

 public:
  // `source` and `presets` must outlive the window. `title` names the install.
  DataMainWindow(processing::IAnalysisSource& source, processing::PresetStore& presets, const QString& title,
                 QWidget* parent = nullptr);
  ~DataMainWindow() override;

  DataWorkspace* workspace() const noexcept { return data_; }
  DataBrowserWindow* browser() const noexcept { return data_->existing_browser(); }
  // File > Installations…; hidden until a handler is set.
  void set_installations_handler(std::function<void()> handler);
  QAction* installations_action() const noexcept { return installations_; }

 private:
  DataWorkspace* data_;
  QAction* installations_;
  std::function<void()> on_installations_;
};

}  // namespace pychron::ui
