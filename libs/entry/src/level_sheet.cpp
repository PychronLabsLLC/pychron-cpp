#include "pychron/entry/level_sheet.hpp"

#include <algorithm>

#include "pychron/entry/names.hpp"

namespace pychron::entry {

namespace ps = persistence;

LevelSheetEdit::LevelSheetEdit(ps::LevelSheet sheet, std::optional<ps::HolderValue> holder) : sheet_(std::move(sheet)) {
  level_note_ = sheet_.level.note;
  holder_ = sheet_.level.holder;
  if (sheet_.z) z_ = sheet_.z->z;
  if (sheet_.production_value) production_ = sheet_.production_value->production;
  geometry_object_ = sheet_.geometry ? sheet_.geometry->ref_object : ps::Uuid::v7();
  production_object_ = sheet_.production ? sheet_.production->ref_object : ps::Uuid::v7();
  for (const auto& p : sheet_.positions) {
    SheetRow r;
    r.position = p.position;
    r.stored = p;
    r.sample = p.sample;
    r.sample_name = p.sample_name;
    r.project = p.project;
    r.principal_investigator = p.principal_investigator;
    r.material = p.material;
    r.grainsize = p.grainsize;
    r.weight = p.weight;
    r.packet = p.packet;
    r.note = p.note;
    rows_.push_back(std::move(r));
  }
  place_holes(holder);
}

void LevelSheetEdit::place_holes(const std::optional<ps::HolderValue>& holder) {
  // Drop placeholder rows; keep every row that holds something.
  std::erase_if(rows_, [](const SheetRow& r) { return r.empty(); });
  const int holes = holder ? static_cast<int>(holder->holes.size()) : 0;
  for (auto& r : rows_) {
    r.hole_id.reset();
    r.orphan = holder.has_value() && r.position > holes;
  }
  if (holder) {
    for (const auto& h : holder->holes) {
      const int position = h.ordinal + 1;
      SheetRow* r = find(position);
      if (!r) {
        rows_.push_back(SheetRow{});
        r = &rows_.back();
        r->position = position;
      }
      if (holder->has_hole_numbers) r->hole_id = h.hole_id;
    }
  }
  std::sort(rows_.begin(), rows_.end(), [](const SheetRow& a, const SheetRow& b) { return a.position < b.position; });
}

SheetRow* LevelSheetEdit::find(int position) {
  for (auto& r : rows_)
    if (r.position == position) return &r;
  return nullptr;
}

const SheetRow* LevelSheetEdit::row(int position) const {
  for (const auto& r : rows_)
    if (r.position == position) return &r;
  return nullptr;
}

void LevelSheetEdit::add_row(int position) {
  if (position < 1 || find(position)) return;
  SheetRow r;
  r.position = position;
  rows_.push_back(std::move(r));
  std::sort(rows_.begin(), rows_.end(), [](const SheetRow& a, const SheetRow& b) { return a.position < b.position; });
}

void LevelSheetEdit::assign_sample(const std::vector<int>& positions, const ps::SampleRow& sample) {
  for (int p : positions) {
    SheetRow* r = find(p);
    if (!r) continue;
    r->sample = sample.uuid;
    r->sample_name = sample.name;
    r->project = sample.project_name;
    r->principal_investigator = sample.principal_investigator_name;
    r->material = sample.material_name;
    r->grainsize = sample.grainsize;
  }
}

void LevelSheetEdit::clear(const std::vector<int>& positions, const std::set<SheetField>& fields) {
  for (int p : positions) {
    SheetRow* r = find(p);
    if (!r) continue;
    if (fields.contains(SheetField::Sample)) {
      r->sample.reset();
      r->sample_name.clear();
      r->project.clear();
      r->principal_investigator.clear();
      r->material.clear();
      r->grainsize.clear();
    }
    if (fields.contains(SheetField::Weight)) r->weight.reset();
    if (fields.contains(SheetField::Packet)) r->packet.reset();
    if (fields.contains(SheetField::Note)) r->note.reset();
  }
}

void LevelSheetEdit::set_weight(int position, std::optional<double> weight) {
  if (SheetRow* r = find(position)) r->weight = weight;
}
void LevelSheetEdit::set_packet(int position, std::optional<std::string> packet) {
  if (SheetRow* r = find(position)) r->packet = packet && !packet->empty() ? packet : std::nullopt;
}
void LevelSheetEdit::set_note(int position, std::optional<std::string> note) {
  if (SheetRow* r = find(position)) r->note = note && !note->empty() ? note : std::nullopt;
}

Result<void> LevelSheetEdit::fill_packets(const std::vector<int>& positions, const std::string& first) {
  if (!valid_packet(first)) return fail(ErrorKind::Config, "packet '" + first + "': letters, then digits (P7)");
  std::vector<int> sorted = positions;
  std::sort(sorted.begin(), sorted.end());
  std::string packet = first;
  for (int p : sorted) {
    if (!find(p)) continue;
    set_packet(p, packet);
    packet = *next_packet(packet);
  }
  return {};
}

Result<void> LevelSheetEdit::move(int from, int to) {
  SheetRow* a = find(from);
  if (!a) return fail(ErrorKind::Config, "no position " + std::to_string(from));
  if (a->analyzed())
    return fail(ErrorKind::Config, "position " + std::to_string(from) + " is analyzed or loaded; it cannot move");
  SheetRow* b = find(to);
  if (!b) {
    add_row(to);
    a = find(from);
    b = find(to);
  }
  if (!b->empty())
    return fail(ErrorKind::Config, "position " + std::to_string(to) + " is not empty");
  const std::optional<std::string> hole_id = b->hole_id;
  const bool orphan = b->orphan;
  SheetRow moved = *a;
  moved.position = to;
  moved.hole_id = hole_id;
  moved.orphan = orphan;
  SheetRow left;
  left.position = from;
  left.hole_id = a->hole_id;
  left.orphan = a->orphan;
  *b = std::move(moved);
  *a = std::move(left);
  // An emptied orphan is not a hole: drop its row.
  std::erase_if(rows_, [&](const SheetRow& r) { return r.position == from && r.empty() && r.orphan; });
  return {};
}

void LevelSheetEdit::set_level_note(std::optional<std::string> note) {
  level_note_ = note && !note->empty() ? note : std::nullopt;
}
void LevelSheetEdit::set_holder(std::optional<ps::Uuid> holder, std::optional<ps::HolderValue> value) {
  holder_ = holder;
  place_holes(value);
}
void LevelSheetEdit::set_z(std::optional<double> z) { z_ = z; }
void LevelSheetEdit::set_production(std::optional<ps::Uuid> production) { production_ = production; }

namespace {

bool row_changed(const SheetRow& r) {
  if (!r.stored) return !r.empty();
  const ps::PositionRow& s = *r.stored;
  return r.position != s.position || r.sample != s.sample || r.weight != s.weight || r.packet != s.packet ||
         r.note != s.note;
}

}  // namespace

bool LevelSheetEdit::dirty() const {
  if (level_note_ != sheet_.level.note || holder_ != sheet_.level.holder) return true;
  if (z_ != (sheet_.z ? sheet_.z->z : std::nullopt)) return true;
  if (production_ != (sheet_.production_value ? std::optional<ps::Uuid>{sheet_.production_value->production} : std::nullopt))
    return true;
  return std::any_of(rows_.begin(), rows_.end(), row_changed);
}

std::vector<std::string> LevelSheetEdit::validate(const EntrySettings& settings) const {
  std::vector<std::string> out;
  for (const auto& r : rows_) {
    const std::string at = "position " + std::to_string(r.position);
    if (r.stored && r.stored->identifier && !r.sample)
      out.push_back(at + " has identifier " + *r.stored->identifier + " and needs a sample");
    if (r.packet && !valid_packet(*r.packet)) out.push_back(at + ": packet '" + *r.packet + "' is letters then digits");
    if (settings.null_identifier_rows == "packet" && r.sample && !(r.stored && r.stored->identifier) && !r.packet)
      out.push_back(at + " needs a packet");
  }
  return out;
}

int LevelSheetEdit::analyses_changing_sample() const {
  int n = 0;
  for (const auto& r : rows_)
    if (r.stored && r.stored->n_analyses > 0 && r.sample != r.stored->sample) n += r.stored->n_analyses;
  return n;
}

ps::CatalogEditBatch LevelSheetEdit::to_batch() const {
  ps::CatalogEditBatch batch;
  batch.message = "level " + sheet_.irradiation_name + "/" + sheet_.level.name;
  const auto opt_text = [](const std::optional<std::string>& v) { return v ? ps::CatalogValue{*v} : ps::CatalogValue{}; };
  const auto opt_num = [](const std::optional<double>& v) { return v ? ps::CatalogValue{*v} : ps::CatalogValue{}; };
  const auto opt_id = [](const std::optional<ps::Uuid>& v) { return v ? ps::CatalogValue{*v} : ps::CatalogValue{}; };

  ps::CatalogUpdate level{ps::CatalogTable::Level, sheet_.level.uuid, {}, {}};
  if (level_note_ != sheet_.level.note) {
    level.expected["note"] = opt_text(sheet_.level.note);
    level.values["note"] = opt_text(level_note_);
  }
  if (holder_ != sheet_.level.holder) {
    level.expected["holder_ref_uuid"] = opt_id(sheet_.level.holder);
    level.values["holder_ref_uuid"] = opt_id(holder_);
  }
  if (!level.values.empty()) batch.edits.emplace_back(std::move(level));

  // Moves first, to holes nobody holds, so a swap of stored rows never clashes.
  std::vector<ps::CatalogEdit> moves, others;
  for (const auto& r : rows_) {
    if (!row_changed(r)) continue;
    if (!r.stored) {
      ps::CatalogFields values{{"level_uuid", sheet_.level.uuid}, {"position", static_cast<std::int64_t>(r.position)}};
      if (r.sample) values["sample_uuid"] = *r.sample;
      if (r.weight) values["weight"] = *r.weight;
      if (r.packet) values["packet"] = *r.packet;
      if (r.note) values["note"] = *r.note;
      others.emplace_back(ps::CatalogInsert{ps::CatalogTable::IrradiationPosition, ps::Uuid::v7(), std::move(values)});
      continue;
    }
    const ps::PositionRow& s = *r.stored;
    ps::CatalogUpdate u{ps::CatalogTable::IrradiationPosition, s.uuid, {}, {}};
    if (r.position != s.position) {
      u.expected["position"] = static_cast<std::int64_t>(s.position);
      u.values["position"] = static_cast<std::int64_t>(r.position);
    }
    if (r.sample != s.sample) {
      u.expected["sample_uuid"] = opt_id(s.sample);
      u.values["sample_uuid"] = opt_id(r.sample);
    }
    if (r.weight != s.weight) {
      u.expected["weight"] = opt_num(s.weight);
      u.values["weight"] = opt_num(r.weight);
    }
    if (r.packet != s.packet) {
      u.expected["packet"] = opt_text(s.packet);
      u.values["packet"] = opt_text(r.packet);
    }
    if (r.note != s.note) {
      u.expected["note"] = opt_text(s.note);
      u.values["note"] = opt_text(r.note);
    }
    (r.position != s.position ? moves : others).emplace_back(std::move(u));
  }
  for (auto& e : moves) batch.edits.push_back(std::move(e));
  for (auto& e : others) batch.edits.push_back(std::move(e));

  const std::string key = sheet_.irradiation_name + "/" + sheet_.level.name;
  const bool z_changed = z_ != (sheet_.z ? sheet_.z->z : std::nullopt);
  if (z_changed && !sheet_.geometry)
    batch.edits.emplace_back(ps::CatalogInsert{ps::CatalogTable::RefObject, geometry_object_,
                                            {{"ref_type", std::string("level_geometry")}, {"key", key},
                                             {"irradiation_uuid", sheet_.level.irradiation},
                                             {"level_uuid", sheet_.level.uuid}}});
  const bool production_changed =
      production_ && production_ != (sheet_.production_value ? std::optional<ps::Uuid>{sheet_.production_value->production}
                                                             : std::nullopt);
  if (production_changed && !sheet_.production)
    batch.edits.emplace_back(ps::CatalogInsert{ps::CatalogTable::RefObject, production_object_,
                                            {{"ref_type", std::string("level_production")}, {"key", key},
                                             {"irradiation_uuid", sheet_.level.irradiation},
                                             {"level_uuid", sheet_.level.uuid}}});
  return batch;
}

Result<bool> LevelSheetEdit::stage_refs(ps::IUnitOfWork& uow) const {
  bool staged = false;
  if (z_ != (sheet_.z ? sheet_.z->z : std::nullopt)) {
    const std::optional<ps::Uuid> head = sheet_.geometry ? sheet_.geometry->revision : std::nullopt;
    if (auto r = uow.add_revision(geometry_object_, ps::Kind::RefValue, ps::RevisionPayload{ps::RefPayload{ps::LevelZValue{z_}}}, head); !r)
      return fail(r.error());
    staged = true;
  }
  if (production_ &&
      production_ != (sheet_.production_value ? std::optional<ps::Uuid>{sheet_.production_value->production} : std::nullopt)) {
    const std::optional<ps::Uuid> head = sheet_.production ? sheet_.production->revision : std::nullopt;
    ps::LevelProductionValue value{*production_, std::nullopt};
    if (auto r = uow.add_revision(production_object_, ps::Kind::RefValue, ps::RevisionPayload{ps::RefPayload{value}}, head); !r)
      return fail(r.error());
    staged = true;
  }
  return staged;
}

}  // namespace pychron::entry
