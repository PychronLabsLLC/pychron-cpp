#include "data_workspace.hpp"

#include <filesystem>
#include <utility>

#include "figure_window.hpp"
#include "isotope_evolution_window.hpp"
#include "pychron/processing/report.hpp"
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
  export_channel_ = 0;
}

bool DataWorkspace::export_report(const QString& path, const QStringList& uuids) {
  if (!processing_) return false;
  if (export_channel_ == 0) export_channel_ = processing_->new_channel();
  const auto& reg = processing::UnitRegistry::builtin();
  processing::Pipeline pipeline;
  pipeline.name = "export";
  auto& select = pipeline.add(reg, "select", "select");
  std::vector<std::string> ids;
  for (const auto& id : uuids) ids.push_back(id.toStdString());
  (void)select.options.set("uuids", ids);
  (void)select.options.set("remove_tags", std::vector<std::string>{});  // the user picked these
  pipeline.add(reg, "reduce", "reduce", {"select"});
  // Two groupings: the report takes the one that fits what came back.
  pipeline.add(reg, "by_aliquot", "group", {"reduce"});
  (void)pipeline.find("by_aliquot")->options.set("key", std::string("aliquot"));
  pipeline.add(reg, "by_identifier", "group", {"reduce"});
  (void)pipeline.find("by_identifier")->options.set("key", std::string("identifier"));
  if (browser_) browser_->show_message(tr("Exporting %n analyses...", nullptr, static_cast<int>(uuids.size())));
  processing_->submit(export_channel_, pipeline, {"by_aliquot", "by_identifier"}, [this, path](const PipelineResult& r) {
    const auto say = [this](const QString& text) {
      if (browser_) browser_->show_message(text);
      if (log_) log_(QStringLiteral("[ui] ") + text);
    };
    if (r.outputs.size() < 2 || !r.outputs[0] || !r.outputs[1]) {
      const auto& bad = r.outputs.empty() ? Error{} : (!r.outputs[0] ? r.outputs[0].error() : r.outputs[1].error());
      say(tr("Export failed: %1").arg(QString::fromStdString(bad.what)));
      return;
    }
    const auto& by_aliquot = std::get<processing::DatasetPtr>(r.outputs[0]->at(0));
    const auto& by_identifier = std::get<processing::DatasetPtr>(r.outputs[1]->at(0));
    bool steps = false;
    for (const auto& it : by_aliquot->items()) steps = steps || it.analysis->analysis->increment >= 0;
    const processing::Dataset& dataset = steps ? *by_aliquot : *by_identifier;
    const auto report = processing::make_report(dataset, processing::ReportOptions{});
    if (auto saved = processing::save_report(report, std::filesystem::path(path.toStdString())); !saved) {
      say(tr("Export failed: %1").arg(QString::fromStdString(saved.error().what)));
      return;
    }
    say(tr("Wrote %1: %n analyses, %2 groups", nullptr, static_cast<int>(report.analyses.rows.size()))
            .arg(path)
            .arg(report.summary.rows.size()));
  });
  return true;
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
    connect(browser_, &DataBrowserWindow::export_requested, this,
            [this](const QString& path, const QStringList& ids) { export_report(path, ids); });
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
