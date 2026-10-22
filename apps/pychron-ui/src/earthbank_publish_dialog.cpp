#include "earthbank_publish_dialog.hpp"

#include <utility>

#include <QHBoxLayout>
#include <QPushButton>
#include <QVBoxLayout>

namespace pychron::ui {

EarthbankPublishDialog::EarthbankPublishDialog(std::vector<processing::AnalysisPtr> analyses, EarthbankSetup setup,
                                               SettingsFactory settings, QWidget* parent)
    : QDialog(parent),
      panel_(new EarthbankPublishPanel(std::move(analyses), std::move(setup), std::move(settings))),
      run_(new QPushButton(tr("Run"))) {
  setWindowTitle(tr("Publish to EarthBank"));
  resize(980, 720);
  auto* close = new QPushButton(tr("Close"));
  auto* bottom = new QHBoxLayout;
  bottom->addStretch();
  bottom->addWidget(run_);
  bottom->addWidget(close);
  auto* layout = new QVBoxLayout(this);
  layout->addWidget(panel_);
  layout->addLayout(bottom);

  run_->setEnabled(panel_->can_run());
  connect(panel_, &EarthbankPublishPanel::controls_changed, this, [this] { run_->setEnabled(panel_->can_run()); });
  connect(run_, &QPushButton::clicked, this, [this] { panel_->start(panel_->current_action()); });
  connect(close, &QPushButton::clicked, this, &QDialog::close);
}

}  // namespace pychron::ui
