#pragma once

// The source adapter for the legacy MySQL catalog (legacy ingestion spec,
// sections 4.2 and 10). It does not talk to MySQL: it reads the directory
// tools/legacy_dump_to_jsonl.py writes from a mysqldump file, one
// <Table>.jsonl per table and a MANIFEST.json, and turns the catalog tables
// into catalog items, in this order (a table the dump lacks is skipped):
//
//   table                      item                 natural key
//   PrincipalInvestigatorTbl   PiItem               last_name, first_initial
//   ProjectTbl                 ProjectItem          name, principal investigator
//   MaterialTbl                MaterialItem         name, grainsize
//   SampleTbl                  SampleItem           name, project, material
//   IrradiationTbl             IrradiationItem      name
//   LevelTbl                   LevelItem            irradiation, name
//   IrradiationPositionTbl     PositionItem         irradiation, level, position
//                                                   (and the identifier in it)
//   UserTbl                    UserItem             name
//   MassSpectrometerTbl        MassSpecItem         name, lower case
//   ExtractDeviceTbl           ExtractDeviceItem    name
//   LoadTbl                    LoadItem             name
//   LoadPositionTbl            LoadPositionItem     load, position, identifier
//
// Rows name their parents by legacy id; items name them by natural key. A
// table name is matched without regard to case (MySQL on Windows lower-cases
// them); column names are matched as written. A string key (a user, load or
// spectrometer name, an identifier) is matched as MySQL's default collations
// match it, without regard to case or trailing spaces; what is stored is the
// parent's own spelling, of two rows that spell one name the first.
//
// One row is one unit, and yields its item or one conflict. A row the store
// cannot hold is refused: an `identity_clash` conflict, and the import goes
// on. Refused are a row whose required parent (a sample's project, a level's
// irradiation, a position's level, a load position's load and identifier) is
// missing or was itself refused, a row without a required
// value, one with a value that cannot be read, an identifier already placed
// elsewhere, and a row with the natural key of an earlier row and other
// values (the earlier row is kept). The conflict's path is
// "<file>#<legacy id>" at commit <manifest sha256>; its detail holds the
// table, the legacy id, the reason and the row.
//
// A link the store can do without (a project's principal investigator, a
// position's sample, a load's user) that names no usable row does not cost
// the row: it is sent without the link, with an `identity_clash` conflict at
// path "<file>#<legacy id>@<column>" whose detail also holds the column, its
// value and "imported": true. So one sample that cannot be stored does not
// take its positions, their identifiers and their loads with it.
//
// A sample's material is such a link in the legacy database and required in
// the store: a sample that names no material, or none that can be used, is
// sent under the material ingest::kPlaceholderMaterial (grainsize ""), with
// the same kind of conflict at "<file>#<legacy id>@materialID" (spec 10.41).
//
// A text column that holds an optional link or free text and reads
// "---------" (legacy NULL_STR) or only white space has no value; a name
// that is a natural key is kept as written (spec 10.40).
//
// Each batch also lists, as `superseded`, the conflicts its rows could have
// and do not: a store imported under an older rule may hold them.
//
// A row that repeats an earlier one exactly is not a conflict. Rows that name
// a refused duplicate resolve to the row that was kept.
//
// Times: a TIMESTAMP column is UTC when the dump set the session time zone to
// +00:00, as mysqldump does. The type is the one the dump's CREATE TABLE
// gives; a dump without one (--no-create-info) is read with the types the
// legacy ORM declares (TIMESTAMP: an irradiation's and a load's create_date).
// Every other time is naive local time in the lab's zone; of an ambiguous one
// the earlier instant is taken, of one in a gap the instant the gap begins
// (ingest/tz.hpp). MySQL's zero date is no value.
//
// What is produced depends on the directory alone, never on where the stream
// was cut or resumed. The adapter never touches the store;
// ingest::BatchWriter writes what it produces.

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/ingest/adapter.hpp"
#include "pychron/persistence/ids.hpp"

namespace pychron::dvc {

struct CatalogAdapterConfig {
  std::filesystem::path dir;  // what tools/legacy_dump_to_jsonl.py wrote
  std::string lab_time_zone;  // IANA; legacy DATETIME values are naive local time
  int batch_rows = 2000;      // rows per batch; how the stream is cut does not change what is stored
};

class CatalogAdapter final : public ingest::ISourceAdapter {
 public:
  // Reads the whole catalog (it is small). A directory without MANIFEST.json
  // (no conversion, or one that did not finish), a table file that is missing
  // or does not have the rows the manifest counts, a time zone the platform
  // does not know and batch_rows < 1 are errors.
  static Result<std::unique_ptr<CatalogAdapter>> open(CatalogAdapterConfig config);
  ~CatalogAdapter() override;

  // kind legacy_db, url the absolute directory, head the manifest's sha256
  // (of the dump file).
  Result<ingest::SourceDescription> describe() override;
  // A token is "<table index>:<row index>@<manifest sha256>": the next row to
  // send, tables counted in the order above. A token of another dump (the
  // directory was converted again), or one that names no row, starts from the
  // first row; everything already stored is then a no-op. Returns the rows
  // still to send. The state is not asked anything.
  Result<int> plan(std::optional<std::string> resume_token, ingest::IImportState& state) override;
  Result<std::optional<ingest::ImportBatch>> next_batch() override;
  // One unit per row of each table: commit the manifest's sha256, path
  // "<file>#<legacy id>", blob the SHA-256 of its line. The state is not asked
  // anything.
  //   a row that was sent        Imported: the catalog row its item names by natural key, and the
  //                              conflict at "<file>#<legacy id>@<column>" of each link it lost
  //   a row that was refused     Conflict: the conflict at "<file>#<legacy id>"
  // A row that repeats an earlier one exactly names the same catalog row.
  Result<void> for_each_unit(ingest::IImportState& state,
                             const std::function<Result<void>(const ingest::SourceUnit&)>& visit) override;
  // A dump has no walk order, and its batches hold no revisions: always nullopt.
  Result<std::optional<std::int64_t>> order_of(std::string_view commit) override;

  // What is worth telling the operator about the dump without stopping the
  // import, one line each. Now: the manifest says the dump has no
  // "-- Dump completed" line, so it may be cut short (mysqldump
  // --skip-comments also writes none).
  std::vector<std::string> warnings() const;

 private:
  class Impl;
  explicit CatalogAdapter(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

// The tag of each analysis as the legacy database has it, for
// ProjectAdapterConfig::tag_lookup: later legacy versions kept tags only in
// MySQL (spec section 10.3). Built from AnalysisTbl.jsonl (id, uuid) and
// AnalysisChangeTbl.jsonl (analysisID, tag) of the same directory; when an
// analysis has several change rows the one with the highest
// idanalysischangeTbl counts. A uuid is read with or without dashes, in
// either case. An analysis without a row, a tag or a readable uuid is not in
// the lookup. A directory without the two tables gives an empty lookup; one
// that has a table file but no MANIFEST.json (a conversion that did not
// finish) is an error, as is a line that is not JSON.
Result<std::function<std::optional<std::string>(const persistence::Uuid&)>> load_tag_lookup(
    const std::filesystem::path& dir);

}  // namespace pychron::dvc
