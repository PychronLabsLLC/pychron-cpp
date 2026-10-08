#pragma once

// The flux window (flux window design, sections 5.3 and 5.4): the irradiation
// levels on the left, and for the one picked the plot of its monitors over the
// tables of monitors, of the selected monitor's analyses and of the unknowns.
// A level is read from the store on the bridge's worker and fitted here, on the
// GUI thread; the window computes nothing itself (the scene, the status line
// and the warnings are libs/processing's).
//
// The fit is edited from the plot and the check boxes (decision W9: one change
// to `Edits`, redrawn from the refit) and from the "Fit" dock on the right: the
// monitor group, which belongs to the level and reloads it, the presets, and
// the options editor. Edits pending are asked about before they are left
// behind (section 5.6).
//
// Save (section 5.5) writes the fit on show as one changeset, on the bridge's
// worker like every store call (W10), and by save_level's rules alone (W11).

#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <QMainWindow>
#include <QString>
#include <QStringList>

#include "entry_bridge.hpp"
#include "pychron/core/error.hpp"
#include "pychron/processing/flux_fit.hpp"
#include "pychron/processing/options.hpp"
#include "pychron/processing/source.hpp"

class QAction;
class QCheckBox;
class QCloseEvent;
class QComboBox;
class QLabel;
class QLineEdit;
class QTableView;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

namespace pychron::ui {

class FluxAnalysisModel;
class FluxMonitorModel;
class FluxUnknownModel;
class OptionsEditor;
class PresetBar;
class SceneView;

class FluxWindow : public QMainWindow {
  Q_OBJECT

 public:
  // All three must outlive the window. A level is read through `source` on the
  // bridge's worker, so `source` must also outlive the jobs the window started:
  // destroy the bridge (which finishes them) before the source.
  FluxWindow(EntryBridge& bridge, processing::IAnalysisSource& source, processing::PresetStore& presets,
             QWidget* parent = nullptr);
  ~FluxWindow() override;

  // How the window asks what to do with edits pending (tests answer without a
  // dialog); a message box with the three buttons by default.
  enum class Unsaved { Save, Discard, Cancel };
  void set_ask_unsaved(std::function<Unsaved(const QString&)> ask) { ask_unsaved_ = std::move(ask); }

  void reload_tree();                                                  // asynchronous
  // Asynchronous; asks first when edits are pending.
  void open_level(const QString& irradiation, const QString& level);
  // A bridge job is running, or a refit is pending.
  bool busy() const noexcept;
  QString status() const;
  bool status_is_error() const noexcept { return status_error_; }
  QStringList warnings() const { return warnings_; }  // the status tooltip, one per line

  QTreeWidget* tree() const noexcept { return tree_; }
  SceneView* view() const noexcept { return view_; }
  FluxMonitorModel* monitors() const noexcept { return monitors_; }
  FluxUnknownModel* unknowns() const noexcept { return unknowns_; }
  FluxAnalysisModel* analyses() const noexcept { return analyses_; }
  QTableView* monitor_table() const noexcept { return monitor_table_; }
  const processing::LevelInputs* inputs() const noexcept { return inputs_ ? &*inputs_ : nullptr; }  // nullptr until a level loaded
  const processing::LevelFit* fit() const noexcept { return fit_ ? &*fit_ : nullptr; }  // nullptr when the fit failed
  // The options of the last fit asked for; when the editor holds options that
  // are no fit's (sd with a surface), the last that were.
  const processing::FluxOptions& options() const noexcept { return options_; }
  void select_monitor(int hole);  // as clicking its row

  // Section 5.6: the edits, the options or a Save box differ from what the load
  // produced, or the monitor group from what the level gives when nothing is
  // chosen for it (also once the level was read with the choice).
  bool edited() const noexcept;
  const processing::Edits& edits() const noexcept { return edits_; }
  // As a plot click or a rubber band: each analysis that takes part is left
  // out and each that is left out takes part again. One that was not reduced
  // or has no J, and a uuid the level does not know, is passed over.
  void toggle_analyses(const QStringList& uuids);
  void set_in_fit(int hole, bool in_fit);                  // as the Fit box
  void set_options(const processing::Options& options);    // as editing the dock
  OptionsEditor* options_editor() const noexcept { return editor_; }
  PresetBar* preset_bar() const noexcept { return preset_bar_; }
  QComboBox* monitor_set_combo() const noexcept { return set_combo_; }
  QLineEdit* sample_edit() const noexcept { return sample_edit_; }
  QCheckBox* all_positions_box() const noexcept { return all_box_; }

  // The loaded options, no edits, every Save box ticked. The store is not read,
  // but when another monitor group was chosen: the level is then read again as
  // its saved fit chose its monitors (asynchronous, and nothing is asked).
  void revert();
  void reload();           // asks, then reads the level again with the monitor group in force
  // Nothing of the edits made here, and Edits::reset_omits when the saved fit
  // left something out (else there is nothing to forget, and nothing pending).
  void reset_omissions();
  QAction* revert_action() const noexcept { return revert_action_; }
  QAction* reload_action() const noexcept { return reload_action_; }
  QAction* reset_omissions_action() const noexcept { return reset_action_; }

  // Section 5.5. Asynchronous: the fit on show (a refit still pending is made
  // first) is saved as one changeset, but for the positions whose Save box is
  // unticked. Nothing happens without a fit, or while a level is read or saved.
  void save();
  void set_save(int hole, bool save);  // as a Save box
  QAction* save_action() const noexcept { return save_action_; }
  // The fit on show as flux_csv_rows writes it, under its header; an error
  // without a fit and for a file that cannot be written.
  Result<void> export_csv(const QString& path);
  QAction* export_action() const noexcept { return export_action_; }
  int loads_started() const noexcept { return loads_started_; }  // level reads asked of the store

 Q_SIGNALS:
  // The level's J was written (not: there was nothing to write).
  void saved(const QString& irradiation, const QString& level);

 protected:
  void closeEvent(QCloseEvent* event) override;

 private:
  // The monitor group: the set, the sample the monitors are chosen by (the
  // set's own when the field is empty) and "all positions".
  struct MonitorGroup {
    std::string set, sample;
    bool all_positions = false;
    friend bool operator==(const MonitorGroup&, const MonitorGroup&) = default;
  };

  void build_dock();
  void start_load();                              // of irradiation_ / level_, with chosen_
  void apply_loaded(processing::LevelInputs inputs, std::vector<processing::MonitorSet> sets, std::string default_set);
  void clear_level();                             // nothing on show; the models let go of inputs_ and fit_
  void request_fit();                             // fit_now() 150 ms after the last request
  void fit_now();
  void show_selected();                           // the analyses table, of selected_hole_
  void update_scene();
  void update_title();
  void set_status(const QString& text, bool error, const QStringList& warnings = {});
  void say(const QString& text, bool error);      // in place of the status line, the warnings kept
  void forget_message();                          // the preset bar's, at the user's next change
  void update_tooltip();
  void refresh_status();                          // the status with what follows it: changed elsewhere, edited
  void update_enabled();                          // what waits for a load or a save
  void update_actions();
  void restore_selection();                       // the row of selected_hole_, after a model was reset

  bool pending(bool with_group) const;            // edited(), with or without the monitor group
  bool group_edited() const;                      // a monitor group was chosen that is not the level's own
  // The analysis used or left out as asked (nullopt: the other way round);
  // false when nothing changed.
  bool set_used(const std::string& uuid, std::optional<bool> use);
  Unsaved ask();
  // Runs `next` when nothing is pending or the user discards it; otherwise
  // `stay` (when given) puts back what the user had changed to get here.
  void leave(bool with_group, std::function<void()> next, const std::function<void()>& stay);
  // Saves; `next` (when given) runs in place of the reload once the level was
  // saved or had nothing to save, and not at all otherwise.
  void save_then(std::function<void()> next);
  void export_asked();

  void options_changed();                         // values_ changed: resolve and refit
  void resolve_options();                         // values_ -> options_ or options_error_
  void show_preset();                             // the preset bar as the level was loaded
  void apply_skip();

  void set_monitor_sets(std::vector<processing::MonitorSet> sets, std::string default_set);
  const processing::MonitorSet* monitor_set(const QString& name) const;
  MonitorGroup group_shown() const;               // what the group's widgets say
  void show_group(const MonitorGroup& group);     // without that being the user's change
  void update_set_hint();                         // the combo's tooltip and the sample's placeholder
  void group_changed();
  QTreeWidgetItem* tree_item(const QString& irradiation, const QString& level) const;
  void select_tree_item();                        // the item of irradiation_ / level_, without loading it

  EntryBridge& bridge_;
  processing::IAnalysisSource& source_;
  processing::PresetStore& presets_;

  QTreeWidget* tree_ = nullptr;
  SceneView* view_ = nullptr;
  FluxMonitorModel* monitors_ = nullptr;
  FluxUnknownModel* unknowns_ = nullptr;
  FluxAnalysisModel* analyses_ = nullptr;
  QTableView* monitor_table_ = nullptr;
  QTableView* analysis_table_ = nullptr;
  QTableView* unknown_table_ = nullptr;
  QLabel* status_ = nullptr;
  QTimer* fit_timer_ = nullptr;
  PresetBar* preset_bar_ = nullptr;
  OptionsEditor* editor_ = nullptr;
  QComboBox* set_combo_ = nullptr;
  QLineEdit* sample_edit_ = nullptr;
  QCheckBox* all_box_ = nullptr;
  QWidget* dock_host_ = nullptr;
  QAction* save_action_ = nullptr;
  QAction* export_action_ = nullptr;
  QAction* revert_action_ = nullptr;
  QAction* reload_action_ = nullptr;
  QAction* reset_action_ = nullptr;
  std::function<Unsaved(const QString&)> ask_unsaved_;

  // The level asked for (empty: none yet), and what was read and made of it.
  // The models point into inputs_, fit_ and unfitted_: they are reset before
  // any of the three changes.
  QString irradiation_, level_;
  std::optional<processing::LevelInputs> inputs_;
  // The options as the editor holds them and as the load left them, and what
  // they are to fit_level: options_, or options_error_ when they are no fit's.
  processing::Options values_, loaded_values_;
  processing::FluxOptions options_;
  std::string options_error_;
  processing::Edits edits_;
  std::set<int> skip_;  // the holes whose Save box is unticked
  std::optional<processing::LevelFit> fit_;
  std::string fit_error_;
  std::optional<int> selected_hole_;
  // The selected monitor when there is no fit: its analyses as fit_level would have counted them.
  std::optional<processing::FittedPosition> unfitted_;

  // The monitor sets of the store's document, for the combo.
  std::vector<processing::MonitorSet> sets_;
  std::string default_set_;
  // The user's choice for this level (nullopt: as its saved fit chose), and
  // what the group shows that is not the user's doing: the loaded selection,
  // or the one being loaded.
  std::optional<MonitorGroup> chosen_;
  MonitorGroup shown_;
  // The group the level shows read with nothing chosen (nullopt: not read so
  // yet): what a chosen group is an edit against.
  std::optional<MonitorGroup> baseline_;
  // The preset in use, which a level without a saved fit opens on; and the one
  // this level opened on (empty: its saved fit).
  QString preset_name_, loaded_preset_;
  std::optional<int> reselect_;  // the monitor selected when the level was read again

  int busy_ = 0;       // bridge jobs outstanding
  int tree_jobs_ = 0;  // of them, tree reads
  quint64 load_generation_ = 0, tree_generation_ = 0;  // a result of an older request is dropped
  bool resetting_ = false;  // the monitors table is being reset: its selection signals are not the user's
  bool syncing_ = false;    // the monitor group is being set: its signals are not the user's
  bool asking_ = false;     // the unsaved question is up (a line edit losing focus to it says "finished" again)
  bool loading_ = false;    // a level is being read
  bool saving_ = false;     // a save is running
  bool notifying_ = false;  // the bridge's changed() is this window's own, after its save
  int loads_started_ = 0;
  // What a save said, shown once the level it reloaded (load `note_generation_`) is fitted.
  QString note_;
  quint64 note_generation_ = 0;
  QString tree_error_;      // the last tree read failed with this
  bool changed_elsewhere_ = false;  // the store changed under edits that were kept
  bool status_error_ = false;
  QString status_text_;     // the status without what refresh_status() adds
  QString message_, message_details_;  // what the preset bar said, after the status until the next change
  QStringList warnings_;
};

}  // namespace pychron::ui
