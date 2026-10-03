#pragma once

// An IStore that forwards every call to another store, for tests that need
// one call to fail. A new IStore method needs a forwarder here.

#include <functional>
#include <memory>
#include <utility>

#include "pychron/persistence/store.hpp"

namespace pychron::ingest::testing {

// clang-format off
class ForwardingStore : public persistence::IStore {
 public:
  explicit ForwardingStore(persistence::IStore& inner) : inner_(inner) {}

  // Called before each begin_import_batch with its 1-based ordinal; an error
  // returned is the call's result.
  std::function<Result<void>(int)> before_import_batch;

  Result<std::unique_ptr<persistence::IImportUnitOfWork>> begin_import_batch(persistence::Uuid source,
                                                                             persistence::Uuid client) override {
    ++import_batches_;
    if (before_import_batch)
      if (auto r = before_import_batch(import_batches_); !r) return fail(r.error());
    return inner_.begin_import_batch(source, client);
  }

 private:
  using Uuid = persistence::Uuid;
  using Kind = persistence::Kind;
  using Actor = persistence::Actor;

 public:
  persistence::Dialect dialect() const noexcept override { return inner_.dialect(); }
  Result<std::vector<persistence::AppliedMigration>> schema_status() override { return inner_.schema_status(); }
  Result<std::unique_ptr<persistence::IUnitOfWork>> begin(const Actor& actor) override { return inner_.begin(actor); }
  Result<persistence::ImportSourceInfo> begin_import(const persistence::ImportSourceSpec& spec) override { return inner_.begin_import(spec); }
  Result<std::vector<persistence::ImportSourceInfo>> import_sources() override { return inner_.import_sources(); }
  Result<std::vector<persistence::ImportConflictRow>> import_conflicts(const persistence::ConflictFilter& filter) override { return inner_.import_conflicts(filter); }
  Result<std::optional<persistence::ImportConflictRow>> import_conflict(Uuid conflict) override { return inner_.import_conflict(conflict); }
  Result<std::vector<persistence::ProvenanceRow>> provenance_for(Uuid entity) override { return inner_.provenance_for(entity); }
  Result<bool> has_provenance(Uuid source, std::string_view commit_sha, std::string_view path) override { return inner_.has_provenance(source, commit_sha, path); }
  Result<bool> has_provenance_blob(Uuid source, std::string_view path, std::string_view git_blob_sha) override { return inner_.has_provenance_blob(source, path, git_blob_sha); }
  Result<bool> has_conflict(Uuid source, std::string_view path, const Sha256Digest& file_sha256) override { return inner_.has_conflict(source, path, file_sha256); }
  Result<std::optional<std::string>> imported_head_blob_sha(Uuid source, Uuid subject, Kind kind) override { return inner_.imported_head_blob_sha(source, subject, kind); }
  Result<Uuid> register_client(const persistence::ClientRegistration& registration) override { return inner_.register_client(registration); }
  Result<Uuid> ensure_user(Uuid client, const std::string& name) override { return inner_.ensure_user(client, name); }
  Result<Uuid> add_mass_spectrometer(Uuid client, const persistence::MassSpectrometerSpec& spec) override { return inner_.add_mass_spectrometer(client, spec); }
  Result<Uuid> add_identifier(Uuid client, const persistence::IdentifierSpec& spec) override { return inner_.add_identifier(client, spec); }
  Result<Uuid> add_extract_device(Uuid client, const std::string& name) override { return inner_.add_extract_device(client, name); }
  Result<Uuid> add_principal_investigator(Uuid client, const persistence::PrincipalInvestigatorSpec& spec) override { return inner_.add_principal_investigator(client, spec); }
  Result<Uuid> add_project(Uuid client, const persistence::ProjectSpec& spec) override { return inner_.add_project(client, spec); }
  Result<Uuid> add_material(Uuid client, const persistence::MaterialSpec& spec) override { return inner_.add_material(client, spec); }
  Result<Uuid> add_sample(Uuid client, const persistence::SampleSpec& spec) override { return inner_.add_sample(client, spec); }
  Result<Uuid> add_irradiation(Uuid client, const std::string& name) override { return inner_.add_irradiation(client, name); }
  Result<Uuid> add_level(Uuid client, const persistence::LevelSpec& spec) override { return inner_.add_level(client, spec); }
  Result<Uuid> add_irradiation_position(Uuid client, const persistence::PositionSpec& spec) override { return inner_.add_irradiation_position(client, spec); }
  Result<Uuid> add_ref_object(Uuid client, const persistence::RefObjectSpec& spec) override { return inner_.add_ref_object(client, spec); }
  Result<Uuid> add_load(Uuid client, const persistence::LoadSpec& spec) override { return inner_.add_load(client, spec); }
  Result<void> add_load_position(Uuid client, const persistence::LoadPositionSpec& spec) override { return inner_.add_load_position(client, spec); }
  Result<Uuid> add_interpreted_age(Uuid client, const persistence::InterpretedAgeSpec& spec) override { return inner_.add_interpreted_age(client, spec); }
  Result<std::optional<Uuid>> find_identifier(const std::string& identifier) override { return inner_.find_identifier(identifier); }
  Result<std::optional<Uuid>> find_analysis(const std::string& identifier, int aliquot, int increment) override { return inner_.find_analysis(identifier, aliquot, increment); }
  Result<std::optional<std::string>> identifier_at(const std::string& irradiation, const std::string& level, int position) override { return inner_.identifier_at(irradiation, level, position); }
  Result<Uuid> add_repository(Uuid client, const std::string& name) override { return inner_.add_repository(client, name); }
  Result<void> add_repository_members(const Actor& actor, Uuid repository, const std::vector<Uuid>& analyses) override { return inner_.add_repository_members(actor, repository, analyses); }
  Result<Uuid> create_group(const Actor& actor, const std::string& name, const std::vector<Uuid>& analyses, std::optional<Uuid> uuid) override { return inner_.create_group(actor, name, analyses, uuid); }
  Result<Uuid> create_bookmark(const Actor& actor, const persistence::BookmarkSpec& spec) override { return inner_.create_bookmark(actor, spec); }
  Result<std::vector<persistence::HeadInfo>> bookmark_heads(Uuid bookmark) override { return inner_.bookmark_heads(bookmark); }
  Result<persistence::CommitOutcome> restore_bookmark(const Actor& actor, Uuid bookmark, std::string message) override { return inner_.restore_bookmark(actor, bookmark, std::move(message)); }
  Result<persistence::CommitOutcome> rollback_to_collection(const Actor& actor, Uuid analysis, std::string message, std::vector<Kind> kinds) override { return inner_.rollback_to_collection(actor, analysis, std::move(message), std::move(kinds)); }
  Result<persistence::RefResolution> resolve_refs(Uuid analysis, const persistence::RefPolicy& policy) override { return inner_.resolve_refs(analysis, policy); }
  Result<Sha256Digest> input_fingerprint(Uuid analysis, const std::string& reduction_version) override { return inner_.input_fingerprint(analysis, reduction_version); }
  Result<void> put_derived(Uuid analysis, const Sha256Digest& fingerprint, const std::string& reduction_version, const std::vector<persistence::DerivedRow>& rows) override { return inner_.put_derived(analysis, fingerprint, reduction_version, rows); }
  Result<std::optional<std::vector<persistence::DerivedRow>>> get_derived(Uuid analysis, const std::string& reduction_version) override { return inner_.get_derived(analysis, reduction_version); }
  Result<int> prune_derived(Uuid analysis) override { return inner_.prune_derived(analysis); }
  Result<persistence::IngestAck> ingest(const persistence::IngestItem& item) override { return inner_.ingest(item); }
  Result<std::optional<Uuid>> head(Uuid subject, Kind kind) override { return inner_.head(subject, kind); }
  Result<std::vector<persistence::HeadInfo>> heads(Uuid subject) override { return inner_.heads(subject); }
  Result<std::vector<persistence::RevisionInfo>> history(Uuid subject, Kind kind) override { return inner_.history(subject, kind); }
  Result<std::optional<persistence::RevisionPayload>> load_payload(Uuid revision) override { return inner_.load_payload(revision); }
  Result<std::optional<persistence::AnalysisView>> load_analysis(Uuid analysis) override { return inner_.load_analysis(analysis); }
  Result<std::vector<persistence::AnalysisSummary>> find_analyses(const persistence::AnalysisQuery& query) override { return inner_.find_analyses(query); }
  Result<persistence::BrowseResult> browse(const persistence::BrowseRequest& request) override { return inner_.browse(request); }
  Result<std::vector<std::string>> facet(persistence::BrowseFacet facet, const persistence::BrowseFilter& filter) override { return inner_.facet(facet, filter); }
  Result<std::optional<persistence::AnalysisDetail>> load_analysis_detail(Uuid analysis) override { return inner_.load_analysis_detail(analysis); }
  Result<std::optional<persistence::BlobData>> load_blob(const Sha256Digest& sha) override { return inner_.load_blob(sha); }
  Result<persistence::ChangePage> changes_since(persistence::ChangeSeq cursor, int limit) override { return inner_.changes_since(cursor, limit); }
  Result<persistence::ChangeSeq> latest_change_seq() override { return inner_.latest_change_seq(); }

 private:
  persistence::IStore& inner_;
  int import_batches_ = 0;
};
// clang-format on

}  // namespace pychron::ingest::testing
