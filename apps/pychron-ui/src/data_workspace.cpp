#include "data_workspace.hpp"

#include <utility>

#include "figure_window.hpp"
#include "isotope_evolution_window.hpp"
#include "recall_window.hpp"
#include "reference_fit_window.hpp"

namespace pychron::ui {

DataWorkspace::DataWorkspace(QWidget* owner, std::function<void(const QString&)> log)
    : QObject(owner), owner_(owner), log_(std::move(log)) {}

DataWorkspace::~DataWorkspace() { clear(); }

void DataWorkspace::clear() {
  for (auto& w : children_)
    if (w) delete w.data();  // they hold the old bridge and source
  children_.clear();
  delete browser_;
  browser_ = nullptr;
  processing_.reset();
}

void DataWorkspace::set_source(processing::IAnalysisSource* source, processing::PresetStore* presets) {
  clear();
  source_ = source;
  presets_ = presets;
  if (source != nullptr && presets != nullptr) processing_ = std::make_unique<ProcessingBridge>(*source);
}

void DataWorkspace::set_page_size(int rows) {
  page_size_ = rows;
  if (browser_ != nullptr) browser_->set_page_size(rows);
}

DataBrowserWindow* DataWorkspace::browser(QWidget* embed_in) {
  if (source_ == nullptr || processing_ == nullptr) return nullptr;
  if (browser_ == nullptr) {
    browser_ = new DataBrowserWindow(*source_, owner_);
    browser_->set_page_size(page_size_);
    if (embed_in != nullptr) browser_->setParent(embed_in, Qt::Widget);
    connect(browser_, &DataBrowserWindow::recall_requested, this, [this](const QString& id) { open_recall(id); });
    connect(browser_, &DataBrowserWindow::figure_requested, this,
            [this](const QString& kind, const QStringList& ids) { open_figure(kind, ids); });
  }
  return browser_;
}

QWidget* DataWorkspace::open_recall(const QString& uuid) {
  if (source_ == nullptr) return nullptr;
  auto* w = new RecallWindow(*source_, owner_);
  w->setAttribute(Qt::WA_DeleteOnClose);
  if (!w->show_analysis(uuid) && log_) log_(QStringLiteral("ERROR [ui] recall failed: ") + uuid);
  children_.append(w);
  w->show();
  return w;
}

QWidget* DataWorkspace::open_figure(const QString& kind, const QStringList& uuids) {
  if (!processing_ || presets_ == nullptr) return nullptr;
  if (processing::UnitRegistry::builtin().find(kind.toStdString()) == nullptr) return nullptr;
  if (kind == QLatin1String(IsotopeEvolutionWindow::kKind)) {
    auto* e = new IsotopeEvolutionWindow(*processing_, *presets_, uuids, owner_);
    e->setAttribute(Qt::WA_DeleteOnClose);
    children_.append(e);
    e->show();
    return e;
  }
  if (ReferenceFitWindow::handles(kind.toStdString())) {
    auto* r = new ReferenceFitWindow(*processing_, *presets_, kind.toStdString(), uuids, owner_);
    r->setAttribute(Qt::WA_DeleteOnClose);
    children_.append(r);
    r->show();
    return r;
  }
  auto* w = new FigureWindow(*processing_, *presets_, kind.toStdString(), uuids, owner_);
  w->setAttribute(Qt::WA_DeleteOnClose);
  connect(w, &FigureWindow::recall_requested, this, [this](const QString& id) { open_recall(id); });
  children_.append(w);
  w->show();
  return w;
}

}  // namespace pychron::ui
