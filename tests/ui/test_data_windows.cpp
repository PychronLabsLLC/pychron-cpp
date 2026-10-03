// Data browsing and visualization windows (design section 11) on an
// in-memory source: the browser filters and pages, recall shows every tab,
// the figure window computes on the bridge, a click on a point excludes the
// analysis and the statistics change, the options dock and presets drive the
// figure, and the main window tears the data windows down before the bridge.
// Recall edits fits (pending until saved, saved through a revision source,
// conflicts reported) and shows revision history and diffs.

#include <cmath>
#include <filesystem>
#include <memory>
#include <set>

#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTabBar>
#include <QTabWidget>
#include <QMenu>
#include <QToolButton>
#include <QTableView>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <qcustomplot.h>

#include "data_browser_window.hpp"
#include "figure_window.hpp"
#include "main_window.hpp"
#include "options_editor.hpp"
#include "processing_bridge.hpp"
#include "pychron/processing/time_series.hpp"
#include "recall_window.hpp"
#include "isotope_evolution_window.hpp"
#include "pychron/processing/isotope_classifier.hpp"
#include "preset_bar.hpp"
#include "reference_fit_window.hpp"
#include "scene_view.hpp"
#include "ui_fixture.hpp"

using namespace pychron;
namespace pp = pychron::processing;
using pychron::ui::DataBrowserWindow;
using pychron::ui::FigureWindow;
using pychron::ui::OptionsEditor;
using pychron::ui::ProcessingBridge;
using pychron::ui::RecallWindow;
using pychron::ui::ReferenceFitWindow;
using pychron::ui::IsotopeEvolutionWindow;

namespace {

constexpr int kWaitMs = 10000;

// Air i: Ar40/Ar36 = 295 + (i % 6), one hour apart; every 5th is a blank.
std::shared_ptr<pp::Analysis> analysis(int i) {
  auto a = std::make_shared<pp::Analysis>();
  const bool blank = i % 5 == 4;
  a->identifier = blank ? "bu" : "air";
  a->uuid = "uuid-" + std::to_string(i);
  a->aliquot = i + 1;
  a->runid = pp::make_runid(a->identifier, a->aliquot, -1);
  a->analysis_type = blank ? "blank_unknown" : "air";
  a->timestamp = 1'700'000'000.0 + 3600.0 * i;
  a->mass_spectrometer = i % 2 ? "jan" : "obama";
  a->sample = blank ? "" : "air";
  const double ratio = 295.0 + (i % 6);
  auto iso = [&](const char* name, const char* det, double v, double e) {
    pp::IsotopeData d;
    d.key = name;
    d.isotope = name;
    d.detector = det;
    d.intercept = {v, e};
    d.n = 20;
    reduction::FitSpec f;
    f.kind = reduction::FitKind::Linear;
    d.fit = f;
    a->isotopes.push_back(d);
  };
  iso("Ar40", "H1", 10.0 * ratio, 0.5);
  iso("Ar39", "AX", 1.0, 0.01);
  iso("Ar38", "L1", 2.0, 0.01);
  iso("Ar37", "L2", 0.5, 0.01);
  iso("Ar36", "CDD", 10.0, 0.02);
  a->gains = {{"H1", 1.0}};
  return a;
}

pp::RawData raw_for(const pp::Analysis& a) {
  pp::RawData raw;
  for (const auto& iso : a.isotopes) {
    pp::RawSeries s;
    s.kind = pp::SeriesKind::Signal;
    s.key = iso.key;
    s.detector = iso.detector;
    for (int k = 0; k < 20; ++k) {
      s.t.push_back(k * 2.0);
      s.v.push_back(iso.intercept.value - 0.01 * k);
    }
    raw.series.push_back(s);
  }
  return raw;
}

std::unique_ptr<pp::MemorySource> make_source(int n) {
  auto src = std::make_unique<pp::MemorySource>();
  for (int i = 0; i < n; ++i) {
    auto a = analysis(i);
    src->add(a, raw_for(*a));
  }
  return src;
}

// Eight concordant heating steps (Ar40 = 10 Ar39 + 298.56 Ar36) with a J.
std::unique_ptr<pp::MemorySource> make_steps() {
  auto src = std::make_unique<pp::MemorySource>();
  const double ar39[] = {5, 20, 40, 60, 50, 30, 15, 5};
  const double ar36[] = {0.5, 0.3, 0.2, 0.1, 0.08, 0.06, 0.1, 0.2};
  for (int i = 0; i < 8; ++i) {
    auto a = std::make_shared<pp::Analysis>();
    a->uuid = "step-" + std::to_string(i);
    a->identifier = "S1";
    a->aliquot = 1;
    a->increment = i;
    a->runid = pp::make_runid("S1", 1, i);
    a->analysis_type = "unknown";
    a->timestamp = 1'700'000'000.0 + 1800.0 * i;
    auto iso = [&](const char* name, double v, double e) {
      pp::IsotopeData d;
      d.key = name;
      d.isotope = name;
      d.intercept = {v, e};
      a->isotopes.push_back(d);
    };
    iso("Ar40", 10 * ar39[i] + 298.56 * ar36[i], 0.3);
    iso("Ar39", ar39[i], 0.05);
    iso("Ar38", 0.05, 0.005);
    iso("Ar37", 0.1, 0.005);
    iso("Ar36", ar36[i], 0.003);
    a->context.flux = reduction::Flux{{0.001, 1e-6}, 0.0, std::nullopt};
    src->add(a);
  }
  return src;
}

// Raw data with a baseline series per detector (0.01 + 0.001 k).
pp::RawData raw_with_baselines(const pp::Analysis& a) {
  pp::RawData raw = raw_for(a);
  std::set<std::string> detectors;
  for (const auto& iso : a.isotopes) detectors.insert(iso.detector);
  for (const auto& d : detectors) {
    pp::RawSeries s;
    s.kind = pp::SeriesKind::Baseline;
    s.key = d;
    s.detector = d;
    for (int k = 0; k < 10; ++k) {
      s.t.push_back(k);
      s.v.push_back(0.01 + 0.001 * k);
    }
    raw.series.push_back(s);
  }
  return raw;
}

// A memory source that keeps intercepts and baselines revisions, like the
// store: every analysis starts at a root of each; saves and restores are
// compare-and-swap on the heads.
class RevisionMemorySource : public pp::MemorySource, public pp::IRevisionSource {
 public:
  explicit RevisionMemorySource(int n) {
    for (int i = 0; i < n; ++i) {
      auto a = analysis(i);
      for (auto& iso : a->isotopes) iso.baseline = {0.01, 0.001};
      for (const char* kind : {"intercepts", "baselines", "blanks", "icfactors"}) {
        a->heads[kind] = std::string("root-") + kind + "-" + a->uuid;
        record(*a, kind, a->heads[kind], "collection", "");
      }
      add(a, raw_with_baselines(*a));
    }
  }

  pp::IRevisionSource* revisions() noexcept override { return this; }

  Result<std::vector<pp::RevisionSummary>> history(const std::string& uuid, pp::RevisionKind kind) override {
    auto list = history_[{uuid, std::string(pp::to_string(kind))}];
    std::reverse(list.begin(), list.end());
    for (auto& r : list) r.head = r.id == head_[{uuid, std::string(pp::to_string(kind))}];
    return list;
  }

  Result<pp::RevisionTable> revision_table(const std::string& id) override {
    auto it = tables_.find(id);
    if (it == tables_.end()) return fail(ErrorKind::Config, "no revision " + id);
    return it->second;
  }

  Result<pp::SaveOutcome> save_fits(const std::string& uuid, const std::map<std::string, std::string>& heads,
                                    const std::vector<pp::EditedFit>& edits, const std::string& message) override {
    pp::SaveOutcome out;
    std::set<std::string> kinds;
    for (const auto& e : edits) kinds.insert(e.kind == pp::SeriesKind::Baseline ? "baselines" : "intercepts");
    for (const auto& k : kinds)
      if (heads.at(k) != head_[{uuid, k}]) {
        out.conflict = "someone else saved first";
        return out;
      }
    auto copy = std::make_shared<pp::Analysis>(**load(uuid));
    for (const auto& e : edits)
      for (auto& iso : copy->isotopes) {
        if (e.kind == pp::SeriesKind::Signal && iso.key == e.key) {
          iso.intercept = e.value;
          iso.fit = e.fit;
          iso.n = e.n_used;
          iso.user_excluded = e.user_excluded;
        } else if (e.kind == pp::SeriesKind::Baseline && iso.detector == e.key) {
          iso.baseline = e.value;
          iso.baseline_fit = e.fit;
          iso.baseline_user_excluded = e.user_excluded;
        }
      }
    out.saved = true;
    ++saves;
    for (const auto& k : kinds) {
      const std::string id = "rev-" + std::to_string(++ids_) + "-" + k;
      copy->heads[k] = id;
      out.revisions[k] = id;
      record(*copy, k, id, "reduction", message);
    }
    add(copy);
    last_message = message;
    return out;
  }

  Result<pp::SaveOutcome> restore_revision(const std::string& uuid, pp::RevisionKind kind, const std::string& expected,
                                           const std::string& revision, const std::string& message) override {
    const std::string k(pp::to_string(kind));
    pp::SaveOutcome out;
    if (expected != head_[{uuid, k}]) {
      out.conflict = "someone else saved first";
      return out;
    }
    auto snap = snapshots_.find(revision);
    if (snap == snapshots_.end()) return fail(ErrorKind::Config, "no revision " + revision);
    auto copy = std::make_shared<pp::Analysis>(**load(uuid));
    for (std::size_t i = 0; i < copy->isotopes.size(); ++i) {
      auto& iso = copy->isotopes[i];
      const auto& old = snap->second.isotopes[i];
      if (k == "intercepts") {
        iso.intercept = old.intercept;
        iso.fit = old.fit;
        iso.n = old.n;
        iso.user_excluded = old.user_excluded;
      } else {
        iso.baseline = old.baseline;
        iso.baseline_fit = old.baseline_fit;
        iso.baseline_user_excluded = old.baseline_user_excluded;
      }
    }
    copy->heads[k] = revision;
    head_[{uuid, k}] = revision;
    add(copy);
    last_message = message;
    ++restores;
    out.saved = true;
    out.revisions[k] = revision;
    return out;
  }

  Result<pp::SaveOutcome> save_reference_fits(const pp::ReferenceFitSet& fits) override {
    const std::string k(pp::to_string(fits.target));
    pp::SaveOutcome out;
    for (const auto& a : fits.analyses)
      if (a.heads.count(k) == 0 || a.heads.at(k) != head_[{a.uuid, k}]) {
        out.conflict = "someone else saved first";
        return out;
      }
    for (const auto& a : fits.analyses) {
      auto copy = std::make_shared<pp::Analysis>(**load(a.uuid));
      for (const auto& row : a.rows)
        for (auto& iso : copy->isotopes) {
          if (fits.target == pp::ReferenceFitTarget::Blanks && iso.key == row.key) iso.blank = row.value;
          if (fits.target == pp::ReferenceFitTarget::IcFactors && iso.detector == row.key) iso.ic_factor = row.value;
        }
      const std::string id = "rev-" + std::to_string(++ids_) + "-" + k;
      copy->heads[k] = id;
      out.revisions[a.uuid] = id;
      record(*copy, k, id, "reduction", fits.message());
      add(copy);
    }
    out.saved = true;
    ++reference_saves;
    last_message = fits.message();
    return out;
  }

  Result<pp::SaveOutcome> save_isotope_fits(const pp::IsotopeFitSet& fits) override {
    pp::SaveOutcome out;
    for (const auto& a : fits.analyses)
      for (const auto& refit : a.isotopes) {
        const std::string k = refit.fit.kind == pp::SeriesKind::Baseline ? "baselines" : "intercepts";
        if (a.heads.count(k) == 0 || a.heads.at(k) != head_[{a.uuid, k}]) {
          out.conflict = "someone else saved first";
          return out;
        }
      }
    for (const auto& a : fits.analyses) {
      auto copy = std::make_shared<pp::Analysis>(**load(a.uuid));
      std::set<std::string> kinds;
      for (const auto& refit : a.isotopes)
        for (auto& iso : copy->isotopes) {
          if (refit.fit.kind == pp::SeriesKind::Signal && iso.key == refit.fit.key) {
            iso.intercept = refit.fit.value;
            iso.fit = refit.fit.fit;
            iso.n = refit.fit.n_used;
            iso.user_excluded = refit.fit.user_excluded;
            kinds.insert("intercepts");
          } else if (refit.fit.kind == pp::SeriesKind::Baseline && iso.detector == refit.fit.key) {
            iso.baseline = refit.fit.value;
            iso.baseline_fit = refit.fit.fit;
            iso.baseline_user_excluded = refit.fit.user_excluded;
            kinds.insert("baselines");
          }
        }
      for (const auto& k : kinds) {
        const std::string id = "rev-" + std::to_string(++ids_) + "-" + k;
        copy->heads[k] = id;
        out.revisions[a.uuid + "/" + k] = id;
        record(*copy, k, id, "reduction", fits.message());
      }
      add(copy);
    }
    out.saved = true;
    ++isotope_saves;
    last_message = fits.message();
    return out;
  }

  // Someone else saves: the head moves without this window knowing.
  void move_head(const std::string& uuid, const std::string& kind = "intercepts") {
    head_[{uuid, kind}] = "rev-elsewhere";
  }

  int saves = 0;
  int restores = 0;
  int reference_saves = 0;
  int isotope_saves = 0;
  std::string last_message;

 private:
  void record(const pp::Analysis& a, const std::string& kind, const std::string& id, const std::string& cs,
              const std::string& message) {
    auto& list = history_[{a.uuid, kind}];
    pp::RevisionSummary r;
    r.id = id;
    r.parent = list.empty() ? "" : list.back().id;
    r.kind = *pp::parse_revision_kind(kind);
    r.changeset_kind = cs;
    r.author = "jross";
    r.host = "lab-1";
    r.message = message;
    r.created = a.timestamp + 60.0 * static_cast<double>(list.size());
    r.seq = ++seq_;
    list.push_back(r);
    head_[{a.uuid, kind}] = id;
    snapshots_[id] = a;
    pp::RevisionTable t;
    t.columns = {"value", "fit", "excluded"};
    auto excluded_text = [](const std::vector<std::size_t>& v) {
      std::string out;
      for (auto i : v) out += (out.empty() ? "" : ",") + std::to_string(i);
      return out;
    };
    auto fit_text = [](const std::optional<reduction::FitSpec>& f) {
      return f ? std::string(reduction::to_string(f->kind)) : std::string();
    };
    std::set<std::string> seen;
    for (const auto& iso : a.isotopes) {
      if (kind == "intercepts") {
        t.rows.push_back({iso.key,
                          {QString::number(iso.intercept.value, 'g', 10).toStdString(), fit_text(iso.fit),
                           excluded_text(iso.user_excluded)}});
      } else if (seen.insert(iso.detector).second) {
        t.rows.push_back({iso.detector,
                          {QString::number(iso.baseline.value, 'g', 10).toStdString(), fit_text(iso.baseline_fit),
                           excluded_text(iso.baseline_user_excluded)}});
      }
    }
    tables_[id] = t;
  }

  std::map<std::pair<std::string, std::string>, std::vector<pp::RevisionSummary>> history_;
  std::map<std::pair<std::string, std::string>, std::string> head_;
  std::map<std::string, pp::RevisionTable> tables_;
  std::map<std::string, pp::Analysis> snapshots_;
  std::int64_t seq_ = 0;
  int ids_ = 0;
};

QListWidgetItem* find_item(QListWidget* list, const QString& text) {
  for (int i = 0; i < list->count(); ++i)
    if (list->item(i)->text() == text) return list->item(i);
  return nullptr;
}

bool wait_runs(FigureWindow& w, ProcessingBridge& bridge, int runs) {
  QElapsedTimer t;
  t.start();
  while (w.runs_completed() < runs && t.elapsed() < kWaitMs) {
    bridge.wait_idle(100);
    QTest::qWait(5);
  }
  return w.runs_completed() >= runs;
}

bool wait_runs(IsotopeEvolutionWindow& w, ProcessingBridge& bridge, int runs) {
  QElapsedTimer t;
  t.start();
  while (w.runs_completed() < runs && t.elapsed() < kWaitMs) {
    bridge.wait_idle(100);
    QTest::qWait(5);
  }
  return w.runs_completed() >= runs;
}

bool wait_runs(ReferenceFitWindow& w, ProcessingBridge& bridge, int runs) {
  QElapsedTimer t;
  t.start();
  while (w.runs_completed() < runs && t.elapsed() < kWaitMs) {
    bridge.wait_idle(100);
    QTest::qWait(5);
  }
  return w.runs_completed() >= runs;
}

}  // namespace

class TestDataWindows : public QObject {
  Q_OBJECT

 private slots:
  void browser_lists_filters_and_pages() {
    auto src = make_source(260);
    DataBrowserWindow w(*src);
    QCOMPARE(w.model()->rowCount(), 200);  // first page
    QCOMPARE(w.model()->row(0).uuid, std::string("uuid-259"));  // newest first
    QVERIFY(w.load_more_button()->isEnabled());
    QVERIFY(w.status()->text().contains(QStringLiteral("200 of 260")));
    QTest::mouseClick(w.load_more_button(), Qt::LeftButton);
    QCOMPARE(w.model()->rowCount(), 260);
    QVERIFY(!w.load_more_button()->isEnabled());

    // Facet: only blanks.
    auto* types = w.facet_list(pp::Facet::AnalysisType);
    QVERIFY(types);
    QCOMPARE(types->count(), 2);
    find_item(types, QStringLiteral("blank_unknown"))->setCheckState(Qt::Checked);
    QCOMPARE(w.model()->rowCount(), 52);
    // The type list still offers both values (a facet ignores its own filter).
    QCOMPARE(w.facet_list(pp::Facet::AnalysisType)->count(), 2);
    // ... and the spectrometer list narrows to what blanks have.
    QCOMPARE(w.facet_list(pp::Facet::MassSpectrometer)->count(), 2);

    find_item(w.facet_list(pp::Facet::AnalysisType), QStringLiteral("blank_unknown"))->setCheckState(Qt::Unchecked);
    w.search()->setText(QStringLiteral("bu-0"));
    QVERIFY(w.model()->rowCount() > 0);
    for (const auto& r : w.model()->rows()) QVERIFY(r.runid.rfind("bu-0", 0) == 0);
    w.search()->clear();
    w.date_preset()->setCurrentIndex(1);  // last 24 hours of the newest
    QCOMPARE(w.model()->rowCount(), 25);
  }

  void browser_recall_and_time_series_signals() {
    auto src = make_source(10);
    DataBrowserWindow w(*src);
    QSignalSpy recall(&w, &DataBrowserWindow::recall_requested);
    QSignalSpy series(&w, &DataBrowserWindow::figure_requested);
    w.recall_step(1);
    QCOMPARE(recall.count(), 1);
    QCOMPARE(recall.takeFirst().at(0).toString(), QStringLiteral("uuid-9"));
    w.recall_step(1);
    QCOMPARE(recall.takeFirst().at(0).toString(), QStringLiteral("uuid-8"));
    w.select_rows({0, 2});
    QCOMPARE(w.selected_uuids(), (QStringList{QStringLiteral("uuid-9"), QStringLiteral("uuid-7")}));
    const auto actions = w.plot_button()->menu()->actions();
    QCOMPARE(actions.size(), 8);  // four figures, a separator, isotope evolutions, blanks, IC factors
    QCOMPARE(actions[5]->data().toString(), QStringLiteral("isotope_evolution_fit"));
    QCOMPARE(actions[7]->data().toString(), QStringLiteral("icfactor_fit"));
    actions[2]->trigger();  // Age spectrum
    QCOMPARE(series.count(), 1);
    const auto args = series.takeFirst();
    QCOMPARE(args.at(0).toString(), QStringLiteral("spectrum"));
    QCOMPARE(args.at(1).toStringList().size(), 2);
  }

  void recall_shows_every_tab() {
    auto src = make_source(3);
    RecallWindow w(*src);
    QVERIFY(w.show_analysis(QStringLiteral("uuid-1")));
    QVERIFY(w.title_label()->text().contains(QStringLiteral("air-02")));
    QVERIFY(w.computed_table()->rowCount() > 0);
    QCOMPARE(w.isotope_table()->rowCount(), 5);
    QCOMPARE(w.isotope_table()->item(0, 0)->text(), QStringLiteral("Ar40"));
    const int all_columns = w.isotope_table()->columnCount();
    w.stage_selector()->setCurrentIndex(1);  // intercept only
    QVERIFY(w.isotope_table()->columnCount() < all_columns);
    QCOMPARE(w.isotope_table()->item(0, 4)->text().toDouble(), 2960.0);
    QCOMPARE(w.evolutions()->panel_count(), 5);
    w.evolution_kind()->setCurrentIndex(1);  // no baselines in this data
    QCOMPARE(w.evolutions()->panel_count(), 0);
    QVERIFY(!w.show_analysis(QStringLiteral("nope")));
  }

  void recall_fit_edits_are_pending_without_a_revision_source() {
    auto src = make_source(2);
    RecallWindow w(*src);
    QVERIFY(w.show_analysis(QStringLiteral("uuid-1")));
    QVERIFY(w.fit_editor()->isVisible() || !w.isVisible());
    QCOMPARE(w.fit_isotope()->count(), 5);
    QCOMPARE(w.fit_isotope()->currentText(), QStringLiteral("Ar40"));
    QCOMPARE(w.fit_kind()->currentText(), QStringLiteral("linear"));
    QVERIFY(!w.save_button()->isEnabled());
    QVERIFY(!w.revert_button()->isEnabled());
    const double loaded_ar40 = w.isotope_table()->item(0, 4)->text().toDouble();

    w.fit_kind()->setCurrentIndex(w.fit_kind()->findText(QStringLiteral("average")));
    QVERIFY(w.has_pending_edits());
    QVERIFY(w.title_label()->text().contains(QStringLiteral("Unsaved")));
    QVERIFY(w.edit_status()->text().contains(QStringLiteral("Ar40")));
    QVERIFY(w.revert_button()->isEnabled());
    QVERIFY(!w.save_button()->isEnabled());  // memory sources keep no revisions
    QVERIFY(w.save_button()->toolTip().contains(QStringLiteral("--db")));
    QCOMPARE(w.shown()->find_isotope("Ar40")->fit->kind, reduction::FitKind::Average);
    // The isotope table follows: average of v = I - 0.01 k (k = 0..19) is I - 0.095.
    w.stage_selector()->setCurrentIndex(0);
    w.stage_selector()->setCurrentIndex(1);
    QVERIFY(std::abs(w.isotope_table()->item(0, 4)->text().toDouble() - (loaded_ar40 - 0.095)) < 1e-6);
    QVERIFY(w.isotope_table()->item(0, 2)->text().startsWith(QStringLiteral("average")));

    w.revert_edits();
    QVERIFY(!w.has_pending_edits());
    QCOMPARE(w.fit_kind()->currentText(), QStringLiteral("linear"));

    // A click on a point leaves it out; a second click puts it back and the
    // edit disappears.
    emit w.evolutions()->point_clicked(QStringLiteral("Ar39#3"));
    QVERIFY(w.has_pending_edits());
    QCOMPARE(w.fit_isotope()->currentText(), QStringLiteral("Ar39"));
    QCOMPARE(w.shown()->find_isotope("Ar39")->user_excluded, (std::vector<std::size_t>{3}));
    QCOMPARE(w.shown()->find_isotope("Ar39")->n, 19);
    emit w.evolutions()->points_toggled({QStringLiteral("Ar39#3"), QStringLiteral("junk"), QStringLiteral("Ar99#1")});
    QVERIFY(!w.has_pending_edits());

    // Outlier filter controls.
    w.fit_outliers()->setChecked(true);
    QVERIFY(w.has_pending_edits());
    QVERIFY(w.shown()->find_isotope("Ar39")->fit->outliers.enabled);

    // History needs revisions.
    w.tabs()->setCurrentWidget(w.history_page());
    QVERIFY(w.history_note()->text().contains(QStringLiteral("--db")));
    QCOMPARE(w.revision_list()->rowCount(), 0);

    // Loading another analysis drops pending edits.
    QVERIFY(w.show_analysis(QStringLiteral("uuid-0")));
    QVERIFY(!w.has_pending_edits());
  }

  void recall_saves_fit_edits_and_shows_history() {
    RevisionMemorySource src(2);
    RecallWindow w(src);
    QVERIFY(w.show_analysis(QStringLiteral("uuid-1")));
    QVERIFY(!w.save_button()->isEnabled());
    w.fit_kind()->setCurrentIndex(w.fit_kind()->findText(QStringLiteral("average")));
    emit w.evolutions()->point_clicked(QStringLiteral("Ar40#0"));
    QVERIFY(w.save_button()->isEnabled());
    QVERIFY(w.save_edits());
    QCOMPARE(src.saves, 1);
    QCOMPARE(src.last_message, std::string("<ISOEVO> Ar40 linear -> average 1 excluded"));
    QVERIFY(!w.has_pending_edits());
    QVERIFY(w.edit_status()->text().startsWith(QStringLiteral("Saved")));
    QCOMPARE(w.shown()->heads.at("intercepts"), std::string("rev-1-intercepts"));
    QCOMPARE(w.shown()->find_isotope("Ar40")->user_excluded, (std::vector<std::size_t>{0}));
    QCOMPARE(w.fit_kind()->currentText(), QStringLiteral("average"));

    // History: newest first, the head marked; one revision shows its table,
    // two show the differences.
    w.tabs()->setCurrentWidget(w.history_page());
    QCOMPARE(w.revision_list()->rowCount(), 2);
    QVERIFY(w.revision_list()->item(0, 0)->text().contains(QStringLiteral("●")));
    QCOMPARE(w.revision_list()->item(0, 5)->text(), QStringLiteral("<ISOEVO> Ar40 linear -> average 1 excluded"));
    QCOMPARE(w.revision_list()->item(1, 4)->text(), QStringLiteral("collection"));
    QCOMPARE(w.revision_content()->rowCount(), 5);  // the newest is selected
    QCOMPARE(w.revision_content()->item(0, 2)->text(), QStringLiteral("average"));
    w.revision_list()->selectAll();
    QCOMPARE(w.revision_content()->rowCount(), 5);
    QCOMPARE(w.revision_content()->item(0, 0)->text(), QStringLiteral("Ar40"));
    QCOMPARE(w.revision_content()->item(0, 2)->text(), QStringLiteral("linear → average"));
    QCOMPARE(w.revision_content()->item(0, 3)->text(), QStringLiteral(" → 0"));
    QCOMPARE(w.revision_content()->item(1, 2)->text(), QStringLiteral("linear"));
    QVERIFY(w.history_note()->text().contains(QStringLiteral("1 row")));
    w.history_kind()->setCurrentIndex(w.history_kind()->findText(QStringLiteral("Tag")));
    QCOMPARE(w.revision_list()->rowCount(), 0);
    QVERIFY2(w.history_note()->text().contains(QStringLiteral("No revisions")), qPrintable(w.history_note()->text()));
    w.history_kind()->setCurrentIndex(w.history_kind()->findText(QStringLiteral("Intercepts")));

    // Someone else saves first: nothing is written and the status says so.
    src.move_head("uuid-1");
    w.fit_kind()->setCurrentIndex(w.fit_kind()->findText(QStringLiteral("linear")));
    QVERIFY(w.has_pending_edits());
    QVERIFY(!w.save_edits());
    QCOMPARE(src.saves, 1);
    QVERIFY(w.has_pending_edits());
    QVERIFY(w.edit_status()->text().contains(QStringLiteral("someone else saved first")));
  }

  void recall_edits_baselines_and_restores_from_history() {
    RevisionMemorySource src(2);
    RecallWindow w(src);
    QVERIFY(w.show_analysis(QStringLiteral("uuid-1")));
    w.evolution_kind()->setCurrentIndex(1);  // baselines
    QCOMPARE(w.evolutions()->panel_count(), 5);
    QCOMPARE(w.fit_isotope()->count(), 5);  // one per detector
    QVERIFY(w.fit_isotope()->findText(QStringLiteral("H1")) >= 0);
    w.fit_isotope()->setCurrentIndex(w.fit_isotope()->findText(QStringLiteral("H1")));
    QCOMPARE(w.fit_kind()->currentText(), QStringLiteral("average"));  // the default for baselines

    // Leaving out point 2 averages the other nine: (0.09 + 0.043) / 9.
    emit w.evolutions()->point_clicked(QStringLiteral("H1#2"));
    QVERIFY(w.has_pending_edits());
    QCOMPARE(w.shown()->find_isotope("Ar40")->baseline_user_excluded, (std::vector<std::size_t>{2}));
    QVERIFY(std::abs(w.shown()->find_isotope("Ar40")->baseline.value - 0.133 / 9) < 1e-12);
    QCOMPARE(w.shown()->find_isotope("Ar39")->baseline.value, 0.01);  // AX untouched
    QVERIFY(w.edit_status()->text().contains(QStringLiteral("H1 baseline")));
    w.fit_kind()->setCurrentIndex(w.fit_kind()->findText(QStringLiteral("linear")));
    QVERIFY(std::abs(w.shown()->find_isotope("Ar40")->baseline.value - 0.01) < 1e-12);
    // Signals on another tab still edit as before; both save together.
    w.evolution_kind()->setCurrentIndex(0);
    QCOMPARE(w.fit_isotope()->currentText(), QStringLiteral("Ar40"));
    w.fit_kind()->setCurrentIndex(w.fit_kind()->findText(QStringLiteral("average")));

    QVERIFY(!w.restore_button()->isEnabled());  // pending edits
    QVERIFY(w.save_edits());
    QCOMPARE(src.saves, 1);
    QCOMPARE(src.last_message,
             std::string("<ISOEVO> Ar40 linear -> average, H1 baseline ? -> linear SEM no outlier filter 1 excluded"));
    QCOMPARE(w.shown()->heads.at("baselines"), std::string("rev-1-baselines"));
    QCOMPARE(w.shown()->find_isotope("Ar40")->baseline_fit->kind, reduction::FitKind::Linear);

    // Restore the baselines root from History.
    w.tabs()->setCurrentWidget(w.history_page());
    w.history_kind()->setCurrentIndex(w.history_kind()->findText(QStringLiteral("Baselines")));
    QCOMPARE(w.revision_list()->rowCount(), 2);
    QVERIFY(!w.restore_button()->isEnabled());  // the head is selected
    w.revision_list()->selectRow(1);
    QVERIFY(w.restore_button()->isEnabled());
    QVERIFY(w.restore_selected());
    QCOMPARE(src.restores, 1);
    QVERIFY(src.last_message.rfind("<ROLLBACK> baselines to revision", 0) == 0);
    QCOMPARE(w.shown()->find_isotope("Ar40")->baseline, (pp::Value{0.01, 0.001}));
    QVERIFY(!w.shown()->find_isotope("Ar40")->baseline_fit);
    QCOMPARE(w.shown()->find_isotope("Ar40")->fit->kind, reduction::FitKind::Average);  // intercepts stay
    QCOMPARE(w.revision_list()->rowCount(), 2);
    QVERIFY(w.revision_list()->item(1, 0)->text().contains(QStringLiteral("●")));
    QVERIFY(!w.revision_list()->item(0, 0)->text().contains(QStringLiteral("●")));
    QVERIFY(w.history_note()->text().startsWith(QStringLiteral("Restored")));

    // Someone else moves the head: restoring the newer one is refused.
    src.move_head("uuid-1", "baselines");
    w.revision_list()->selectRow(0);
    QVERIFY(w.restore_button()->isEnabled());
    QVERIFY(!w.restore_selected());
    QCOMPARE(src.restores, 1);
    QVERIFY(w.history_note()->text().contains(QStringLiteral("someone else saved first")));

    // Without revisions there is nothing to restore.
    auto plain = make_source(1);
    RecallWindow v(*plain);
    QVERIFY(v.show_analysis(QStringLiteral("uuid-0")));
    QVERIFY(!v.restore_selected());
    QVERIFY(v.restore_button()->toolTip().contains(QStringLiteral("--db")));
  }

  void reference_fits_find_fit_exclude_and_save() {
    // Ten analyses an hour apart; 4 and 9 are blanks (obama, jan), the
    // rest airs. Unknowns 5 (jan) and 6 (obama).
    RevisionMemorySource src(10);
    QTemporaryDir dir;
    ProcessingBridge bridge(src);
    pp::PresetStore presets(dir.path().toStdString());
    ReferenceFitWindow w(bridge, presets, "blank_fit", {QStringLiteral("uuid-5"), QStringLiteral("uuid-6")});
    QVERIFY(wait_runs(w, bridge, 1));
    QCOMPARE(w.reference_uuids(), (QStringList{QStringLiteral("uuid-9"), QStringLiteral("uuid-4")}));
    QVERIFY(w.fits());
    QCOMPARE(w.fits()->analyses.size(), std::size_t{2});
    // Preceding: blank 4 (Ar40/Ar36 = 299, Ar40 = 2990 less the 0.01
    // baseline) for both unknowns.
    QCOMPARE(w.fits()->analyses[0].rows[0].key, std::string("Ar40"));
    QCOMPARE(w.fits()->analyses[0].rows[0].value.value, 2989.99);
    QCOMPARE(w.fits()->analyses[1].rows[0].value.value, 2989.99);
    QCOMPARE(w.view()->panel_count(), 5);
    QVERIFY(w.status_label()->text().contains(QStringLiteral("2 reference")));
    QVERIFY(w.save_button()->isEnabled());

    // Clicking blank 4 leaves it out: preceding falls back to blank 9.
    emit w.view()->point_clicked(QStringLiteral("uuid-4"));
    QVERIFY(wait_runs(w, bridge, 2));
    QCOMPARE(w.fits()->analyses[0].rows[0].value.value, 2979.99);
    for (const auto& ref : w.fits()->analyses[0].rows[0].references)
      QCOMPARE(ref.excluded, ref.uuid == "uuid-4");
    QVERIFY(w.status_label()->text().contains(QStringLiteral("1 left out")));
    w.toggle_references({QStringLiteral("uuid-5")});  // an unknown, not a reference: no change
    QVERIFY(wait_runs(w, bridge, 3));
    QCOMPARE(w.fits()->analyses[0].rows[0].value.value, 2979.99);

    QVERIFY(w.save());
    QCOMPARE(src.reference_saves, 1);
    QCOMPARE(src.last_message,
             std::string("<BLANKS> fits=Ar40(preceding),Ar39(preceding),Ar38(preceding),Ar37(preceding),Ar36(preceding)"));
    QCOMPARE((*src.load("uuid-5"))->find_isotope("Ar40")->blank.value, 2979.99);
    QVERIFY(wait_runs(w, bridge, 4));
    QVERIFY(w.status_label()->text().contains(QStringLiteral("saved blanks for 2 analyses")));

    // The References table lists both, blank 4 unchecked; checking it
    // puts it back.
    QCOMPARE(w.references_table()->rowCount(), 2);
    QTableWidgetItem* box4 = nullptr;
    for (int r = 0; r < 2; ++r)
      if (w.references_table()->item(r, 3)->data(Qt::UserRole).toString() == QStringLiteral("uuid-4"))
        box4 = w.references_table()->item(r, 3);
    QVERIFY(box4);
    QCOMPARE(box4->checkState(), Qt::Unchecked);
    QCOMPARE(w.references_table()->item(0, 0)->text(), QStringLiteral("bu-10"));  // newest first
    box4->setCheckState(Qt::Checked);
    QVERIFY(wait_runs(w, bridge, 5));
    QCOMPARE(w.fits()->analyses[0].rows[0].value.value, 2989.99);

    // Presets: save the current fits as a user preset, change, reload it.
    auto opts = w.pipeline().find("fit")->options;
    auto rows = opts.rows("isotopes");
    QVERIFY(rows[0].set("fit", std::string("average")).has_value());
    QVERIFY(opts.set_rows("isotopes", rows).has_value());
    w.set_fit_options(opts);
    QVERIFY(wait_runs(w, bridge, 6));
    w.presets()->ask_name = [] { return QStringLiteral("Averages"); };
    QVERIFY(w.presets()->save(true));
    QVERIFY(w.presets()->select(QStringLiteral("Default")));
    QVERIFY(wait_runs(w, bridge, 7));
    QCOMPARE(w.fits()->analyses[0].rows[0].fit, pp::ReferenceFitKind::Preceding);
    QVERIFY(w.presets()->select(QStringLiteral("Averages")));
    QVERIFY(wait_runs(w, bridge, 8));
    QCOMPARE(w.fits()->analyses[0].rows[0].fit, pp::ReferenceFitKind::Average);
    QCOMPARE(w.pipeline().find("fit")->preset, std::string("Averages"));

    // Someone else saves first: nothing written.
    src.move_head("uuid-6", "blanks");
    QVERIFY(!w.save());
    QCOMPARE(src.reference_saves, 1);
    QVERIFY(w.status_label()->text().contains(QStringLiteral("someone else saved first")));

    // A window too narrow for any reference: nothing to fit or save.
    w.hours()->setValue(0.5);
    QVERIFY(w.find_references());
    QVERIFY(wait_runs(w, bridge, 9));
    QVERIFY(w.reference_uuids().isEmpty());
    QCOMPARE(w.references_table()->rowCount(), 0);
    QVERIFY(w.fits()->analyses.empty());
    QVERIFY(!w.save_button()->isEnabled());
    QVERIFY(w.status_label()->toolTip().contains(QStringLiteral("no references")));
  }

  void icfactor_fits_from_airs() {
    RevisionMemorySource src(10);
    QTemporaryDir dir;
    ProcessingBridge bridge(src);
    pp::PresetStore presets(dir.path().toStdString());
    // Blank 4 (obama) as the unknown; the obama airs 0, 2, 6, 8 within 10 h.
    ReferenceFitWindow w(bridge, presets, "icfactor_fit", {QStringLiteral("uuid-4")});
    QVERIFY(wait_runs(w, bridge, 1));
    QCOMPARE(w.reference_types()->text(), QStringLiteral("air"));
    QCOMPARE(w.reference_uuids().size(), 4);
    QCOMPARE(w.fits()->analyses.size(), std::size_t{1});
    const auto& row = w.fits()->analyses[0].rows.at(0);
    QCOMPARE(row.key, std::string("CDD"));
    QCOMPARE(row.reference_detector, std::string("H1"));
    // Baseline-corrected Ar40/Ar36 of the airs (ratios 295, 297, 295, 297,
    // baselines 0.01), averaged, over 295.5.
    double expected = 0;
    for (double r : {295.0, 297.0, 295.0, 297.0}) expected += (10 * r - 0.01) / 9.99 / 295.5 / 4;
    QVERIFY(std::abs(row.value.value - expected) < 1e-12);
    QVERIFY(w.save());
    QCOMPARE((*src.load("uuid-4"))->find_isotope("Ar36")->ic_factor.value, row.value.value);

    // Without revisions there is nothing to save to.
    auto plain = make_source(10);
    ProcessingBridge plain_bridge(*plain);
    ReferenceFitWindow v(plain_bridge, presets, "blank_fit", {QStringLiteral("uuid-5")});
    QVERIFY(wait_runs(v, plain_bridge, 1));
    QVERIFY(!v.save_button()->isEnabled());
    QVERIFY(v.save_button()->toolTip().contains(QStringLiteral("--db")));
    QVERIFY(!v.save());
  }

  void isotope_evolutions_refit_flag_and_save() {
    // Signals are exact lines v = I - 0.01 k: a linear refit keeps I, an
    // average gives I - 0.095.
    RevisionMemorySource src(4);
    QTemporaryDir dir;
    ProcessingBridge bridge(src);
    pp::PresetStore presets(dir.path().toStdString());
    IsotopeEvolutionWindow w(bridge, presets,
                             {QStringLiteral("uuid-0"), QStringLiteral("uuid-1"), QStringLiteral("uuid-2")});
    QVERIFY(wait_runs(w, bridge, 1));
    QVERIFY(w.fits());
    QCOMPARE(w.fits()->analyses.size(), std::size_t{3});
    QCOMPARE(w.fits()->analyses[0].isotopes.size(), std::size_t{5});
    QVERIFY(std::abs(w.fits()->analyses[0].isotopes[0].fit.value.value - 2950.0) < 1e-6);
    QCOMPARE(w.analyses_table()->rowCount(), 3);
    QVERIFY(w.analyses_table()->item(0, 3)->text().startsWith(QStringLiteral("Ar40 2950")));
    QCOMPARE(w.view()->panel_count(), 5);

    // Selecting an analysis previews its evolutions with the refits.
    w.analyses_table()->selectRow(0);
    QCOMPARE(w.preview_view()->panel_count(), 5);

    auto opts = w.pipeline().find("fit")->options;
    auto rows = opts.rows("isotopes");
    QVERIFY(rows[0].set("fit", std::string("average")).has_value());
    QVERIFY(opts.set_rows("isotopes", rows).has_value());
    w.set_fit_options(opts);
    QVERIFY(wait_runs(w, bridge, 2));
    QVERIFY(std::abs(w.fits()->analyses[0].isotopes[0].fit.value.value - 2949.905) < 1e-6);
    QCOMPARE(w.preview_view()->panel_count(), 5);  // still previewing uuid-0

    // Unchecking an analysis leaves it out.
    w.analyses_table()->item(1, 1)->setCheckState(Qt::Unchecked);
    QVERIFY(wait_runs(w, bridge, 3));
    QCOMPARE(w.fits()->analyses.size(), std::size_t{2});
    QCOMPARE(w.analyses_table()->item(1, 3)->text(), QStringLiteral("left out"));

    // A goodness threshold nothing passes flags both.
    QVERIFY(rows[0].set("max_percent_error", 1e-12).has_value());
    QVERIFY(opts.set_rows("isotopes", rows).has_value());
    w.set_fit_options(opts);
    QVERIFY(wait_runs(w, bridge, 4));
    QCOMPARE(w.fits()->flagged(), 2);
    QVERIFY(w.analyses_table()->item(0, 2)->text().startsWith(QStringLiteral("Ar40 percent_error")));
    QVERIFY(w.status_label()->text().contains(QStringLiteral("2 flagged")));
    w.leave_out_flagged()->setChecked(true);
    QVERIFY(!w.save_button()->isEnabled());
    QVERIFY(!w.save());
    w.leave_out_flagged()->setChecked(false);
    QVERIFY(w.save());
    QCOMPARE(src.isotope_saves, 1);
    QCOMPARE(src.last_message,
             std::string("<ISOEVO> refit Ar40(average),Ar39(linear),Ar38(linear),Ar37(linear),Ar36(linear)"));
    QVERIFY(std::abs((*src.load("uuid-0"))->find_isotope("Ar40")->intercept.value - 2949.905) < 1e-6);
    QVERIFY(wait_runs(w, bridge, 5));
    QVERIFY(w.status_label()->text().contains(QStringLiteral("saved intercepts of 2 analyses")));

    // Someone else saves first: nothing written.
    src.move_head("uuid-2", "intercepts");
    QVERIFY(!w.save());
    QCOMPARE(src.isotope_saves, 1);
    QVERIFY(w.status_label()->text().contains(QStringLiteral("someone else saved first")));

    // Without revisions there is nothing to save to.
    auto plain = make_source(3);
    ProcessingBridge plain_bridge(*plain);
    IsotopeEvolutionWindow v(plain_bridge, presets, {QStringLiteral("uuid-0")});
    QVERIFY(wait_runs(v, plain_bridge, 1));
    QCOMPARE(v.fits()->analyses.size(), std::size_t{1});
    QVERIFY(!v.save_button()->isEnabled());
    QVERIFY(v.save_button()->toolTip().contains(QStringLiteral("--db")));
  }

  void isotope_evolutions_refit_baselines() {
    // Baselines are 0.01 + 0.001 k (k = 0..9) on every detector.
    RevisionMemorySource src(2);
    QTemporaryDir dir;
    ProcessingBridge bridge(src);
    pp::PresetStore presets(dir.path().toStdString());
    IsotopeEvolutionWindow w(bridge, presets, {QStringLiteral("uuid-0")});
    QVERIFY(wait_runs(w, bridge, 1));
    auto opts = w.pipeline().find("fit")->options;
    auto rows = opts.rows("isotopes");
    rows.resize(2);  // Ar40 signal, linear; then the H1 baseline
    QVERIFY(rows[1].set("series", std::string("baseline")).has_value());
    QVERIFY(rows[1].set("isotope", std::string("H1")).has_value());
    QVERIFY(rows[1].set("fit", std::string("average")).has_value());
    QVERIFY(rows[1].set("min_rsquared", 0.5).has_value());  // not checked: an average
    QVERIFY(rows[0].set("min_rsquared", 1.0).has_value());  // an R^2 of 1 is still "at or below"
    QVERIFY(opts.set_rows("isotopes", rows).has_value());
    w.set_fit_options(opts);
    QVERIFY(wait_runs(w, bridge, 2));
    const auto& refits = w.fits()->analyses.at(0);
    QCOMPARE(refits.isotopes.size(), std::size_t{2});
    QCOMPARE(refits.isotopes[1].label(), std::string("H1 baseline"));
    QVERIFY(std::abs(refits.isotopes[1].fit.value.value - 0.0145) < 1e-12);
    QVERIFY(w.analyses_table()->item(0, 3)->text().contains(QStringLiteral("H1 baseline 0.0145")));
    QVERIFY(w.analyses_table()->item(0, 2)->text().startsWith(QStringLiteral("Ar40 rsquared 1 <= 1")));

    w.analyses_table()->selectRow(0);
    QCOMPARE(w.preview_view()->panel_count(), 5);  // signals
    w.preview_kind()->setCurrentIndex(1);
    QCOMPARE(w.preview_view()->panel_count(), 5);  // a baseline per detector

    QVERIFY(w.save());
    const auto saved = *src.load("uuid-0");
    QVERIFY(std::abs(saved->find_isotope("Ar40")->baseline.value - 0.0145) < 1e-12);
    QCOMPARE(saved->heads.at("baselines").rfind("rev-", 0), std::size_t{0});
    QVERIFY(src.last_message.find("H1 baseline(average)") != std::string::npos);
  }

  void isotope_evolutions_train_and_use_the_classifier() {
    // uuid-0 has a flat Ar40 sniff, uuid-1 a wildly noisy one.
    pp::MemorySource src;
    for (int i = 0; i < 2; ++i) {
      auto a = analysis(i);
      auto raw = raw_for(*a);
      pp::RawSeries sniff;
      sniff.kind = pp::SeriesKind::Sniff;
      sniff.key = "Ar40";
      for (int k = 0; k < 10; ++k) {
        sniff.t.push_back(0.5 * k);
        sniff.v.push_back(i == 0 ? 100.0 : (k % 2 ? 900.0 : -900.0));
      }
      raw.series.push_back(sniff);
      src.add(a, raw);
    }
    QTemporaryDir dir;
    ProcessingBridge bridge(src);
    pp::PresetStore presets(dir.path().toStdString());
    IsotopeEvolutionWindow w(bridge, presets, {QStringLiteral("uuid-0"), QStringLiteral("uuid-1")});
    const auto file = std::filesystem::path(dir.path().toStdString()) / "classifier" / "isotope.toml";
    w.set_classifier_file(file);
    QVERIFY(wait_runs(w, bridge, 1));
    QVERIFY(!w.train(QStringLiteral("Ar40"), 1));  // nothing previewed yet
    QCOMPARE(w.training_label()->text(), QStringLiteral("0 good, 0 bad"));

    // Each train() re-runs; the bridge coalesces back-to-back runs, so wait for
    // at least one more completion and an idle bridge.
    auto settle = [&](int before) { return wait_runs(w, bridge, before + 1) && bridge.wait_idle(kWaitMs); };
    w.analyses_table()->selectRow(0);
    QCOMPARE(w.train_isotope()->count(), 5);
    int before = w.runs_completed();
    QVERIFY(w.train(QStringLiteral("Ar40"), 1));
    QVERIFY(w.train(QStringLiteral("Ar40"), 1));
    QVERIFY(settle(before));
    w.analyses_table()->selectRow(1);
    before = w.runs_completed();
    QVERIFY(w.train(QStringLiteral("Ar40"), 0));
    QVERIFY(w.train(QStringLiteral("Ar40"), 0));
    QVERIFY(settle(before));
    QCOMPARE(w.training_label()->text(), QStringLiteral("2 good, 2 bad"));
    auto trained = pp::IsotopeClassifier::load(file);
    QVERIFY(trained.has_value());
    QCOMPARE(trained->samples().size(), std::size_t{4});
    QCOMPARE(trained->samples()[0].runid, std::string("air-01"));
    QCOMPARE(w.fits()->flagged(), 0);  // not in use yet

    auto opts = w.pipeline().find("fit")->options;
    QVERIFY(opts.set("use_classifier", true).has_value());
    before = w.runs_completed();
    w.set_fit_options(opts);
    QVERIFY(settle(before));
    QCOMPARE(w.fits()->flagged(), 1);
    const auto& bad = w.fits()->analyses.at(1);
    QCOMPARE(bad.uuid, std::string("uuid-1"));
    QCOMPARE(bad.flags.at(0).check, std::string("classifier"));
    QVERIFY(w.fits()->analyses.at(0).good());
    QVERIFY(w.analyses_table()->item(1, 2)->text().startsWith(QStringLiteral("Ar40 classifier")));

    // Training re-runs with the new samples; switching it off clears the flag.
    before = w.runs_completed();
    QVERIFY(w.train(QStringLiteral("Ar40"), 0));
    QVERIFY(settle(before));
    QCOMPARE(w.training_label()->text(), QStringLiteral("2 good, 3 bad"));
    QCOMPARE(w.fits()->flagged(), 1);
    opts = w.pipeline().find("fit")->options;
    QVERIFY(opts.set("use_classifier", false).has_value());
    before = w.runs_completed();
    w.set_fit_options(opts);
    QVERIFY(settle(before));
    QCOMPARE(w.fits()->flagged(), 0);
  }

  void figure_computes_and_click_excludes() {
    QTemporaryDir dir;
    auto src = make_source(12);
    ProcessingBridge bridge(*src);
    pp::PresetStore presets(dir.path().toStdString());
    QStringList ids;
    for (int i = 0; i < 12; ++i)
      if (i % 5 != 4) ids << QStringLiteral("uuid-%1").arg(i);
    FigureWindow w(bridge, presets, ids);
    w.resize(1000, 800);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    QVERIFY(wait_runs(w, bridge, 1));
    QVERIFY2(w.view()->scene(), qPrintable(w.status_label()->text()));
    QCOMPARE(w.view()->panel_count(), 2);  // Default preset: Ar40, Ar40/Ar36
    QCOMPARE(w.analyses_table()->rowCount(), 10);
    const QStringList before = w.view()->texts(1);
    QVERIFY(!before.isEmpty());

    // Click the analysis with the highest ratio in the ratio panel.
    w.view()->plot()->replot();
    const auto pos = w.view()->point_position("uuid-5", 1);
    QVERIFY(pos);
    QTest::mouseClick(w.view()->plot(), Qt::LeftButton, Qt::NoModifier, w.view()->plot()->mapFrom(w.view(), *pos));
    QVERIFY(wait_runs(w, bridge, 2));
    const auto ex = w.pipeline().find("edits")->options.get_strings("exclude");
    QCOMPARE(ex, std::vector<std::string>{"uuid-5"});
    bool excluded = false;
    for (const auto& it : w.dataset()->items())
      if (it.analysis->analysis->uuid == "uuid-5") excluded = it.exclusion.excluded();
    QVERIFY(excluded);
    QVERIFY(w.view()->texts(1) != before);  // the weighted mean moved

    // Toggling again includes it.
    w.toggle_exclusion({QStringLiteral("uuid-5")});
    QVERIFY(wait_runs(w, bridge, 3));
    QVERIFY(w.pipeline().find("edits")->options.get_strings("exclude").empty());
    QCOMPARE(w.view()->texts(1), before);
  }

  void figure_options_presets_and_export() {
    QTemporaryDir dir;
    auto src = make_source(12);
    ProcessingBridge bridge(*src);
    pp::PresetStore presets((dir.path() + QStringLiteral("/presets")).toStdString());
    QStringList ids;
    for (int i = 0; i < 12; ++i) ids << QStringLiteral("uuid-%1").arg(i);
    FigureWindow w(bridge, presets, ids);
    w.resize(900, 700);
    w.show();
    QVERIFY(wait_runs(w, bridge, 1));

    // The dock edits the figure: one panel, run-number axis.
    auto* editor = w.options_editor();
    QVERIFY(editor->tabs()->count() >= 5);
    QVERIFY(editor->apply(QStringLiteral("x.kind"), std::string("index")));
    QVERIFY(!editor->apply(QStringLiteral("x.kind"), std::string("sideways")));
    QVERIFY(wait_runs(w, bridge, 2));
    QCOMPARE(w.view()->scene()->graphs[0].x.format, pp::AxisFormat::Number);

    // Group by analysis type: two groups, two point layers per panel.
    w.set_group_key(QStringLiteral("analysis_type"));
    QVERIFY(wait_runs(w, bridge, 3));
    int layers = 0;
    for (const auto& l : w.view()->scene()->graphs[0].panels[0].layers) layers += std::holds_alternative<pp::PointLayer>(l);
    QCOMPARE(layers, 2);

    // Save as a user preset, switch to a factory one, come back.
    w.ask_preset_name = [] { return QStringLiteral("My Index"); };
    for (auto* b : w.findChildren<QPushButton*>())
      if (b->text() == QStringLiteral("Save as...")) QTest::mouseClick(b, Qt::LeftButton);
    QVERIFY(w.preset_combo()->findText(QStringLiteral("My Index")) >= 0);
    w.select_preset(QStringLiteral("Air monitor"));
    QVERIFY(wait_runs(w, bridge, 4));
    QCOMPARE(w.view()->panel_count(), 3);
    w.select_preset(QStringLiteral("My Index"));
    QVERIFY(wait_runs(w, bridge, 5));
    QCOMPARE(w.view()->scene()->graphs[0].x.format, pp::AxisFormat::Number);

    const QString png = dir.path() + QStringLiteral("/fig.png");
    const QString pdf = dir.path() + QStringLiteral("/fig.pdf");
    QVERIFY(w.export_figure(png));
    QVERIFY(w.export_figure(pdf));
    QVERIFY(QFileInfo(png).size() > 0);
    QVERIFY(QFileInfo(pdf).size() > 0);
  }

  void arar_figures_open_and_steps_are_clickable() {
    QTemporaryDir dir;
    auto src = make_steps();
    ProcessingBridge bridge(*src);
    pp::PresetStore presets(dir.path().toStdString());
    QStringList ids;
    for (int i = 0; i < 8; ++i) ids << QStringLiteral("step-%1").arg(i);
    for (const char* kind : {"ideogram", "inverse_isochron"}) {
      FigureWindow w(bridge, presets, kind, ids);
      w.resize(900, 700);
      w.show();
      QVERIFY(wait_runs(w, bridge, 1));
      QVERIFY2(w.view()->scene(), qPrintable(w.status_label()->text()));
      QCOMPARE(QString::fromStdString(w.view()->scene()->kind), QString::fromLatin1(kind));
      QVERIFY(w.view()->scene()->warnings.empty());
      QCOMPARE(w.group_combo()->currentText(), FigureWindow::default_group_key(kind));
    }

    FigureWindow w(bridge, presets, "spectrum", ids);
    w.resize(1000, 700);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    QVERIFY(wait_runs(w, bridge, 1));
    QCOMPARE(w.group_combo()->currentText(), QStringLiteral("aliquot"));
    QVERIFY(w.preset_combo()->findText(QStringLiteral("With K/Ca")) >= 0);
    const QStringList before = w.view()->texts(0);
    QVERIFY(before.join(QLatin1Char('\n')).contains(QStringLiteral("plateau A-H")));
    w.view()->plot()->replot();
    const auto pos = w.view()->point_position("step-3", 0);  // the box centre
    QVERIFY(pos);
    QVERIFY(w.view()->tooltip_at(w.view()->plot()->mapFrom(w.view(), *pos)).contains(QStringLiteral("S1-01D")));
    QTest::mouseClick(w.view()->plot(), Qt::LeftButton, Qt::NoModifier, w.view()->plot()->mapFrom(w.view(), *pos));
    QVERIFY(wait_runs(w, bridge, 2));
    QCOMPARE(w.pipeline().find("edits")->options.get_strings("exclude"), std::vector<std::string>{"step-3"});
    QVERIFY(w.view()->texts(0) != before);  // D left the plateau
  }

  void options_editor_rows() {
    OptionsEditor e;
    e.set_options(pp::Options(pp::time_series_schema()));
    auto* rows = e.rows(QStringLiteral("panels"));
    QVERIFY(rows);
    QCOMPARE(rows->count(), 1);
    QSignalSpy changed(&e, &OptionsEditor::changed);
    for (auto* b : rows->parentWidget()->findChildren<QPushButton*>())
      if (b->text() == QStringLiteral("Add")) {
        QTest::mouseClick(b, Qt::LeftButton);
        break;
      }
    QCOMPARE(changed.count(), 1);
    QCOMPARE(e.options().rows("panels").size(), 2u);
    QVERIFY(e.row_editor(QStringLiteral("panels"), QStringLiteral("quantity")));
    // Enabled-when: the fit error is disabled while fit = none.
    QVERIFY(!e.row_editor(QStringLiteral("panels"), QStringLiteral("fit_error"))->isEnabled());
    auto* fit = qobject_cast<QComboBox*>(e.row_editor(QStringLiteral("panels"), QStringLiteral("fit")));
    QVERIFY(fit);
    fit->setCurrentText(QStringLiteral("linear"));
    QVERIFY(e.row_editor(QStringLiteral("panels"), QStringLiteral("fit_error"))->isEnabled());
  }

  void main_window_opens_and_tears_down_data_windows() {
    auto line = ui::test::make_example_line();
    QTemporaryDir dir;
    auto src = make_source(6);
    pp::PresetStore presets(dir.path().toStdString());
    {
      ui::MainWindow window(*line);
      QVERIFY(!window.data_action()->isEnabled());
      window.set_data(src.get(), &presets);
      QVERIFY(window.data_action()->isEnabled());
      window.data_action()->trigger();
      QVERIFY(window.data_window());
      QCOMPARE(window.data_window()->model()->rowCount(), 6);
      QVERIFY(window.open_recall(QStringLiteral("uuid-0")));
      QVERIFY(window.open_time_series({QStringLiteral("uuid-0"), QStringLiteral("uuid-1")}));
      QVERIFY(window.open_figure(QStringLiteral("ideogram"), {QStringLiteral("uuid-0")}));
      QVERIFY(!window.open_figure(QStringLiteral("no_such_figure"), {QStringLiteral("uuid-0")}));
      // Destroyed with a figure window open: data windows go before the bridge.
    }
  }
};

QTEST_MAIN(TestDataWindows)
#include "test_data_windows.moc"
