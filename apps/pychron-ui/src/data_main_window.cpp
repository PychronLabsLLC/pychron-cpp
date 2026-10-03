#include "data_main_window.hpp"

#include <utility>

#include <QAction>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QStatusBar>

namespace pychron::ui {

DataMainWindow::DataMainWindow(processing::IAnalysisSource& source, processing::PresetStore& presets,
                               const QString& title, QWidget* parent)
    : QMainWindow(parent),
      data_(new DataWorkspace(this, [this](const QString& line) { statusBar()->showMessage(line, 10000); })),
      installations_(new QAction(tr("Installations…"), this)) {
  setWindowTitle(QStringLiteral("pychron — %1").arg(title));
  data_->set_source(&source, &presets);
  setCentralWidget(data_->browser(this));

  QMenu* file = menuBar()->addMenu(tr("File"));
  file->addAction(installations_);
  file->addSeparator();
  QAction* quit = file->addAction(tr("Quit"));
  quit->setShortcut(QKeySequence::Quit);
  connect(quit, &QAction::triggered, this, &QMainWindow::close);
  installations_->setVisible(false);
  connect(installations_, &QAction::triggered, this, [this] {
    if (on_installations_) on_installations_();
  });
}

DataMainWindow::~DataMainWindow() {
  takeCentralWidget();  // the workspace deletes the browser with the other data windows
  data_->set_source(nullptr, nullptr);
}

void DataMainWindow::set_installations_handler(std::function<void()> handler) {
  on_installations_ = std::move(handler);
  installations_->setVisible(static_cast<bool>(on_installations_));
}

}  // namespace pychron::ui
