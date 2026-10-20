#pragma once

// RehearsalWindow (spec docs/superpowers/specs/2026-10-20-script-rehearsal-design.md,
// section 6.2): a script stepped one hardware call at a time against a copy of
// the line taken when the rehearsal starts. Nothing is actuated: the canvas is
// read only and paints the rehearsal's copy, never the line.
//
//   top     the run's fields (default_context()'s names), the five a script
//           reads most first, the rest under More
//   left    the canvas over the rehearsal's copy; what the last step changed
//           is outlined
//   right   the script's source, the line it stands at shaded; a gosub's
//           source with the scripts that called it above
//   bottom  the step log: #, where, call, outcome; a read the copy has no
//           value for takes one typed in its outcome cell
//   toolbar Start, Step (F10), Run to End (Shift+F10), Stop, Restart
//
// The rehearsal runs on a RehearsalRunner's thread. What it reports reaches
// this window queued on the window's thread, tagged with the run it belongs
// to, so nothing from a stopped run lands in the next one. The runner is only
// ever destroyed here, on the window's thread, never from its callbacks.

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include <QMainWindow>
#include <QSettings>
#include <QString>

#include "canvas_state.hpp"
#include "core_bridge.hpp"
#include "pychron/experiment/lab/lab.hpp"
#include "pychron/experiment/rehearsal/runner.hpp"
#include "pychron/scripting/script_host.hpp"

class QAction;
class QFormLayout;
class QLabel;
class QLineEdit;
class QTableWidget;
class QWidget;

namespace pychron::ui {

class CanvasView;
class CodeEditor;
class ScriptHighlighter;

// The rehearsal canvas's state: the bridge's mirror of the line as it was
// when the rehearsal started, with valves and switches then taken from the
// rehearsal's copy. Nothing is ever pending; pressures stay as they were.
class RehearsalCanvasState final : public ICanvasState {
  Q_OBJECT

 public:
  // `bridge` must outlive this.
  explicit RehearsalCanvasState(const CoreBridge& bridge, QObject* parent = nullptr);

  // The bridge's state, now.
  void reset();
  // Valves and switches from a rehearsal's copy.
  void apply(const experiment::rehearsal::LineSnapshot& copy);

  [[nodiscard]] const CoreBridge::State& state() const override { return state_; }
  [[nodiscard]] bool pending(const std::string&) const override { return false; }
  [[nodiscard]] QDateTime wall_now() const override { return bridge_.wall_now(); }
  [[nodiscard]] const canvas::Canvas* canvas() const override { return bridge_.canvas(); }
  [[nodiscard]] const systems::NetworkGraph* network() const override { return bridge_.network(); }
  [[nodiscard]] bool gauge_defined(const std::string& name) const override;

 private:
  const CoreBridge& bridge_;
  CoreBridge::State state_;
};

// What a rehearsal starts from: the bridge's switches and valves (a valve's
// state is the bridge's latest), the line's interlocks, its pressures.
experiment::rehearsal::LineSnapshot rehearsal_snapshot(
    const CoreBridge::State& state, std::map<std::string, systems::InterlockSet, std::less<>> interlocks);

class RehearsalWindow : public QMainWindow {
  Q_OBJECT

 public:
  // `host`, `bridge` and `lab` must outlive the window.
  RehearsalWindow(scripting::IScriptHost& host, const CoreBridge& bridge,
                  std::map<std::string, systems::InterlockSet, std::less<>> interlocks,
                  const experiment::lab::Lab& lab, std::unique_ptr<QSettings> settings = nullptr,
                  QWidget* parent = nullptr);
  ~RehearsalWindow() override;  // the runner first
  RehearsalWindow(const RehearsalWindow&) = delete;
  RehearsalWindow& operator=(const RehearsalWindow&) = delete;
  RehearsalWindow(RehearsalWindow&&) = delete;
  RehearsalWindow& operator=(RehearsalWindow&&) = delete;

  // Loads a script, its fields set from `defaults` over make_context()'s.
  // While a rehearsal runs, asks (confirm_replace) first; declined, nothing
  // changes.
  void set_script(scripting::ScriptKind kind, QString name, QString text, scripting::ValueMap defaults = {});
  // A copy of the line and the form's values: a rehearsal. A script that
  // fails its check lists the diagnostics and does not start.
  void start();
  // Lets the step that waits go on. Nothing waits: nothing happens.
  void step();
  // Lets every step go on (to RehearsalRunner's limit) while the step waits.
  void run_to_end();
  // Ends the rehearsal and joins its thread.
  void stop();
  // stop(), then start() over a new copy of the line, with the same fields.
  void restart();

  [[nodiscard]] bool running() const noexcept { return runner_ != nullptr; }
  [[nodiscard]] bool step_enabled() const;
  [[nodiscard]] CanvasView* canvas() const { return canvas_; }
  [[nodiscard]] CodeEditor* source() const { return source_; }
  [[nodiscard]] QTableWidget* log() const { return log_; }  // columns: #, where, call, outcome
  [[nodiscard]] QFormLayout* fields() const { return fields_; }
  [[nodiscard]] QAction* step_action() const { return step_; }
  [[nodiscard]] QAction* run_to_end_action() const { return run_to_end_; }
  [[nodiscard]] QString status_text() const;
  void set_confirm_replace(std::function<bool()> confirm) { confirm_replace_ = std::move(confirm); }

 protected:
  void closeEvent(QCloseEvent* event) override;

 private:
  struct Field {
    std::string name;
    scripting::Value kind;  // the default's alternative says how text reads
    QWidget* editor = nullptr;
  };

  void build_fields();
  void fill_fields(const scripting::ValueMap& values);
  [[nodiscard]] scripting::ValueMap field_values() const;
  void on_record(unsigned run, const experiment::rehearsal::StepRecord& record);
  void on_finished(unsigned run, const Result<scripting::ScriptResult>& result);
  void provide_value(int row, QLineEdit* entry);
  void show_source(const scripting::StepCall& call);
  int add_row(const QString& index, const QString& where, const QString& call, const QString& outcome, bool error);
  void end_runner();  // destroys the runner; its late reports are dropped
  void update_actions();

  scripting::IScriptHost& host_;
  const CoreBridge& bridge_;
  std::map<std::string, systems::InterlockSet, std::less<>> interlocks_;
  const experiment::lab::Lab& lab_;
  std::unique_ptr<QSettings> settings_;
  RehearsalCanvasState canvas_state_;

  scripting::ScriptKind kind_ = scripting::ScriptKind::Extraction;
  QString name_;
  QString text_;
  bool loaded_ = false;
  QString shown_script_;  // the script the source pane holds

  QFormLayout* fields_;
  QFormLayout* more_fields_;
  std::vector<Field> field_list_;
  CanvasView* canvas_;
  QLabel* stack_;
  CodeEditor* source_;
  ScriptHighlighter* highlighter_ = nullptr;  // the source's document's
  QTableWidget* log_;
  QLabel* status_;
  QAction* start_;
  QAction* step_;
  QAction* run_to_end_;
  QAction* stop_;
  QAction* restart_;
  std::function<bool()> confirm_replace_;

  // Which run a report belongs to: bumped at every start and every end.
  unsigned run_ = 0;
  // What the latest report waits for. Step and Run to End are sent only
  // while it is Step, a value only while it is Value: the model keeps a
  // go-ahead sent while nothing waits for the next stop, which would let
  // that step through unasked.
  experiment::rehearsal::Waiting waiting_ = experiment::rehearsal::Waiting::None;
  std::unique_ptr<experiment::rehearsal::RehearsalRunner> runner_;  // last: goes first
};

}  // namespace pychron::ui
