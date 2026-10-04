#pragma once

// The editable positions of one level (sample and package entry spec,
// section 6, level_sheet.hpp). It starts from a stored LevelSheet and the
// level's holder, keeps edits in memory and turns them into one catalog edit
// batch, with the loaded values as expected, plus the reference revisions
// (z, production) staged in a unit of work.

#include <optional>
#include <set>
#include <string>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/entry/settings.hpp"
#include "pychron/persistence/store.hpp"

namespace pychron::entry {

struct SheetRow {
  int position = 0;                         // where the row is now (1-based hole)
  std::optional<std::string> hole_id;       // the holder's id for the hole, when it numbers holes
  std::optional<persistence::PositionRow> stored;  // as loaded; nullopt: nothing stored here
  // The row's current values.
  std::optional<persistence::Uuid> sample;
  std::string sample_name, project, principal_investigator, material, grainsize;
  std::optional<double> weight;
  std::optional<std::string> packet, note;
  bool orphan = false;  // beyond the holder's holes
  bool analyzed() const { return stored && (stored->n_analyses > 0 || stored->in_load); }
  bool empty() const { return !sample && !weight && !packet && !note && !stored; }
};

enum class SheetField { Sample, Weight, Packet, Note };

class LevelSheetEdit {
 public:
  // `holder` is the value of the level's holder reference, if it has one:
  // its holes are the rows, position n being the hole of ordinal n - 1.
  LevelSheetEdit(persistence::LevelSheet sheet, std::optional<persistence::HolderValue> holder);

  const persistence::LevelSheet& loaded() const noexcept { return sheet_; }
  const std::vector<SheetRow>& rows() const noexcept { return rows_; }
  const SheetRow* row(int position) const;

  // A row for a position the holder does not have (no holder, or an extra hole).
  void add_row(int position);
  void assign_sample(const std::vector<int>& positions, const persistence::SampleRow& sample);
  void clear(const std::vector<int>& positions, const std::set<SheetField>& fields);
  void set_weight(int position, std::optional<double> weight);
  void set_packet(int position, std::optional<std::string> packet);
  void set_note(int position, std::optional<std::string> note);
  // `first`, then the packets after it, in position order (P7, P8, ...).
  Result<void> fill_packets(const std::vector<int>& positions, const std::string& first);
  // Moves what is at `from` to the empty hole `to`. Refused when the row is
  // analyzed or loaded.
  Result<void> move(int from, int to);

  void set_level_note(std::optional<std::string> note);
  void set_holder(std::optional<persistence::Uuid> holder, std::optional<persistence::HolderValue> value);
  void set_z(std::optional<double> z);
  void set_production(std::optional<persistence::Uuid> production);

  std::optional<std::string> level_note() const { return level_note_; }
  std::optional<persistence::Uuid> holder() const { return holder_; }
  std::optional<double> z() const { return z_; }
  std::optional<persistence::Uuid> production() const { return production_; }

  bool dirty() const;
  // Problems that block a save: a row with an identifier and no sample, an
  // invalid packet, a missing packet when the settings require one.
  std::vector<std::string> validate(const EntrySettings& settings) const;
  // Analyses whose sample a save would change (the confirmation of E5).
  int analyses_changing_sample() const;

  // The catalog edits; allow_analyzed_sample_change is left false. When z or
  // the production changed and the level has no reference object for it yet,
  // the insert of that object is part of the batch.
  persistence::CatalogEditBatch to_batch() const;
  // Stages the z and production revisions on `uow`; false when neither changed.
  Result<bool> stage_refs(persistence::IUnitOfWork& uow) const;

 private:
  SheetRow* find(int position);
  void place_holes(const std::optional<persistence::HolderValue>& holder);

  persistence::LevelSheet sheet_;
  std::vector<SheetRow> rows_;  // by position
  std::optional<std::string> level_note_;
  std::optional<persistence::Uuid> holder_;
  std::optional<double> z_;
  std::optional<persistence::Uuid> production_;
  persistence::Uuid geometry_object_, production_object_;  // existing, or the uuid a new one gets
};

}  // namespace pychron::entry
