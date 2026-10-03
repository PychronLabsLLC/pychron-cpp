#pragma once

// The source adapter for a legacy Python-pychron MetaData repository (legacy
// ingestion spec, sections 4.3 and 10). It walks the whole history of one
// branch, as the project adapter does, and turns every commit that changes
// reference data into one `reference` changeset:
//
//   file                                   object (type, key)                      revision path
//   <irrad>/<level>.json                   flux_position  <irrad>/<level>/<hole>   <file>#<hole>
//                                          level_geometry <irrad>/<level>          <file>#z
//   <irrad>/productions.json               level_production <irrad>/<level>        <file>#<level>
//   <irrad>/productions/<name>.json        production     <irrad>/<name>           <file>
//   <irrad>/chronology.txt                 chronology     <irrad>                  <file>
//   spectrometers/<name>.gain.json         gains          <name, lower case>       <file>
//   spectrometers/<name>.sens.json         sensitivity    <name, lower case>       <file>#<index>
//   irradiation_holders/<name>.txt         irradiation_holder <name>               <file>
//   load_holders/<name>.txt                load_holder    <name>                   <file>
//
// A file that holds several objects is compared with the version of it before
// it in the walk (the last one that could be read), and only what changed
// gets a revision: the positions of a level whose entry differs, the levels
// of productions.json whose production differs, the entries of a sensitivity
// list that are new or differ. A sensitivity list is used by its last entry;
// when a commit leaves another revision as the head, the last entry is
// restated, so the head is always the entry the legacy code would use.
//
// Every other path is ignored: the repository holds scripts, experiment
// templates and documents that are not reference data. A reference file that
// cannot be read is an `unparseable` conflict and the walk goes on. A deleted
// file, and a position or entry that a new version no longer has, add
// nothing: the object keeps its last value.
//
// It reads through GitReader and never touches the store; ingest::BatchWriter
// writes what it produces. The layout it understands is described in
// tests/dvc/fixtures/README.md, section 6.

#include <memory>
#include <optional>
#include <string>

#include "pychron/core/error.hpp"
#include "pychron/dvc/git_reader.hpp"
#include "pychron/ingest/adapter.hpp"

namespace pychron::dvc {

struct MetaAdapterConfig {
  GitConfig git;              // the repository and branch to read
  std::string url;            // the source's url as registered; ids are derived from its normalized form
  std::string lab_time_zone;  // IANA; chronology and sensitivity times are naive local time
  // Commits per batch. How the walk is cut does not change what is stored.
  int batch_commits = 500;
};

class MetaRepoAdapter final : public ingest::ISourceAdapter {
 public:
  // Opens the repository (GitReader::open); the branch head is fixed here. An
  // empty url or a time zone the platform does not know is an error.
  static Result<std::unique_ptr<MetaRepoAdapter>> open(MetaAdapterConfig config);
  ~MetaRepoAdapter() override;

  Result<ingest::SourceDescription> describe() override;
  // As ProjectRepoAdapter::plan: without a token the walk starts at the first
  // commit; a token that names a commit, its place in the walk order and the
  // commits before it as they are resumes after it; a commit elsewhere in the
  // history starts again from the first; a commit not in the history is an
  // error whose message contains "history was rewritten". The state is not
  // asked anything: what a commit yields depends on the history alone.
  // Returns the commits to walk.
  Result<int> plan(std::optional<std::string> resume_token, ingest::IImportState& state) override;
  Result<std::optional<ingest::ImportBatch>> next_batch() override;

 private:
  class Impl;
  explicit MetaRepoAdapter(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace pychron::dvc
