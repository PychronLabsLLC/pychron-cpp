#pragma once

// One release, several destinations (publication workflow design, section
// 5.1): the plan of a Publish run, the fixed order its steps run in, which
// step is skipped when another failed, and the rules of the plan judged before
// anything runs. Qt-free; running the steps stays in build_release,
// publish_built and the EarthBank code.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/persistence/store.hpp"
#include "pychron/publish/build.hpp"
#include "pychron/publish/release.hpp"

namespace pychron::publish {

enum class Destination { Folder, Store, EarthBank, Doi, Hosted };  // run order
enum class EarthBankMode { Preflight, ExportXlsx, Upload };

struct NewRelease {
  ReleaseSpec spec;  // as create_release takes
};
struct ExistingRelease {
  std::string name_or_uuid;  // find_release text
};

struct FolderStep {
  BuildOptions build;
};
struct StoreStep {
  bool make_public = false;
  std::optional<std::filesystem::path> dir;  // when Folder is not ticked
};
struct EarthBankStep {
  EarthBankMode mode = EarthBankMode::Preflight;
};

struct PublicationPlan {
  std::variant<NewRelease, ExistingRelease> release;
  std::optional<FolderStep> folder;
  std::optional<StoreStep> store;
  std::optional<EarthBankStep> earthbank;
  bool doi = false, hosted = false;  // always refused while not built
};

struct PlanRefusal {
  Destination destination;
  std::string reason;  // one sentence
};

// "Release folder", "Publish in store", "EarthBank", "DOI", "Hosted".
std::string_view destination_name(Destination destination);

// Every rule of section 4 and D3 that can be judged before anything runs, for
// the release as the plan would leave it; every refusal, not the first. Empty:
// the plan may run. Error (not a refusal) only when the store cannot be read.
Result<std::vector<PlanRefusal>> check_plan(persistence::IStore& store, const PublicationPlan& plan, UtcTime now);

// D3 for an EarthBank upload of `release` at `now`: nullopt when it may go,
// else the one-sentence reason. `made_public_in_run`: the same run publishes
// the release publicly (Store ticked with make_public) before the upload.
// Also what the dialog rechecks just before the step, and what elctl asks.
Result<std::optional<std::string>> earthbank_upload_refusal(persistence::IStore& store, persistence::Uuid release,
                                                            bool made_public_in_run, UtcTime now);

enum class StepState { Pending, Running, Ok, Failed, Skipped, Cancelled };
struct StepResult {
  Destination destination;
  StepState state = StepState::Pending;
  std::string message;
};

// Order of section 4 for the destinations ticked.
std::vector<Destination> run_order(const PublicationPlan& plan);

// True when `next` must be skipped: a step it depends on (section 4) is in
// `so_far` as Failed, Skipped or Cancelled. Store depends on Folder, and an
// EarthBank upload on Store and, through it, on Folder, each only when ticked;
// Pre-flight and xlsx on nothing.
bool depends_on_failed(const PublicationPlan& plan, Destination next, const std::vector<StepResult>& so_far);

}  // namespace pychron::publish
