#include "pychron/publish/publication.hpp"

#include <algorithm>
#include <set>

#include "members.hpp"

namespace pychron::publish {

namespace ps = persistence;

namespace {

// A release as a plan leaves it, for the rules: what a stored row says, or
// what create_release would make of a new one.
struct Subject {
  ps::ReleaseState state = ps::ReleaseState::Draft;
  ps::Visibility visibility = ps::Visibility::Private;
  std::optional<UtcTime> embargo_until;
  std::vector<Uuid> members;
};

std::optional<std::string> upload_refusal(ps::IStore& store, const Subject& s, bool made_public_in_run, UtcTime now,
                                          Error* error) {
  if (s.state == ps::ReleaseState::Withdrawn) return "The release is withdrawn, so it cannot go to EarthBank.";
  if (s.state == ps::ReleaseState::Draft) {
    if (!made_public_in_run)
      return "The release is a draft: EarthBank upload needs it published and public, so tick Publish in store with public.";
  } else if (s.visibility != ps::Visibility::Public && !made_public_in_run) {
    return "The release is published but private: EarthBank upload needs it public.";
  }
  if (s.embargo_until && *s.embargo_until > now) return "The release is under embargo until " + s.embargo_until->iso() + ".";
  auto hidden = store.embargoed_analyses(s.members, now);
  if (!hidden) {
    *error = hidden.error();
    return std::nullopt;
  }
  if (!hidden->empty())
    return std::to_string(hidden->size()) + " member(s) of the release are under live embargo, so it cannot be uploaded.";
  return std::nullopt;
}

Subject subject_of(const ps::ReleaseRow& row, std::vector<Uuid> members) {
  return Subject{row.state, row.visibility, row.embargo_until, std::move(members)};
}

}  // namespace

std::string_view destination_name(Destination destination) {
  switch (destination) {
    case Destination::Folder: return "Release folder";
    case Destination::Store: return "Publish in store";
    case Destination::EarthBank: return "EarthBank";
    case Destination::Doi: return "DOI";
    case Destination::Hosted: return "Hosted";
  }
  return "";
}

std::vector<Destination> run_order(const PublicationPlan& plan) {
  std::vector<Destination> order;
  if (plan.folder) order.push_back(Destination::Folder);
  if (plan.store) order.push_back(Destination::Store);
  if (plan.earthbank) order.push_back(Destination::EarthBank);
  if (plan.doi) order.push_back(Destination::Doi);
  if (plan.hosted) order.push_back(Destination::Hosted);
  return order;
}

bool depends_on_failed(const PublicationPlan& plan, Destination next, const std::vector<StepResult>& so_far) {
  const auto failed = [&](Destination d) {
    return std::ranges::any_of(so_far, [&](const StepResult& r) {
      return r.destination == d &&
             (r.state == StepState::Failed || r.state == StepState::Skipped || r.state == StepState::Cancelled);
    });
  };
  switch (next) {
    case Destination::Store: return plan.folder && failed(Destination::Folder);
    case Destination::EarthBank:
      if (!plan.earthbank || plan.earthbank->mode != EarthBankMode::Upload) return false;
      return (plan.store && failed(Destination::Store)) || (plan.store && plan.folder && failed(Destination::Folder));
    default: return false;
  }
}

Result<std::optional<std::string>> earthbank_upload_refusal(ps::IStore& store, Uuid release, bool made_public_in_run,
                                                            UtcTime now) {
  auto row = store.release(release);
  if (!row) return fail(row.error());
  if (!*row) return fail(ErrorKind::Config, "no such release: " + release.str());
  auto members = detail::release_members(store, **row);
  if (!members) return fail(members.error());
  Error error;
  auto refusal = upload_refusal(store, subject_of(**row, std::move(*members)), made_public_in_run, now, &error);
  if (!error.what.empty()) return fail(error);
  return refusal;
}

Result<std::vector<PlanRefusal>> check_plan(ps::IStore& store, const PublicationPlan& plan, UtcTime now) {
  std::vector<PlanRefusal> out;
  const auto order = run_order(plan);
  // A refusal about the release itself belongs to the first ticked
  // destination, or to the release folder when none is ticked.
  const Destination first = order.empty() ? Destination::Folder : order.front();
  const auto refuse = [&](Destination d, std::string reason) { out.push_back({d, std::move(reason)}); };

  if (plan.doi) refuse(Destination::Doi, "Not built yet.");
  if (plan.hosted) refuse(Destination::Hosted, "Not built yet.");
  if (plan.store && !plan.folder && !plan.store->dir)
    refuse(Destination::Store, "Publish in store needs the release folder ticked or a built directory named.");

  std::optional<Subject> subject;
  bool published_already = false;
  if (const auto* existing = std::get_if<ExistingRelease>(&plan.release)) {
    auto row = find_release(store, existing->name_or_uuid);
    if (!row) {
      if (row.error().kind != ErrorKind::Config) return fail(row.error());
      refuse(first, "There is no release \"" + existing->name_or_uuid + "\".");
    } else {
      auto members = detail::release_members(store, *row);
      if (!members) return fail(members.error());
      published_already = row->state != ps::ReleaseState::Draft;
      subject = subject_of(*row, std::move(*members));
    }
  } else {
    const auto& spec = std::get<NewRelease>(plan.release).spec;
    if (spec.analyses.empty()) {
      refuse(first, "The new release has no analyses.");
    } else {
      // Conservative: every candidate counts as a member (create_release's
      // tag filter is not applied), and a new release is a private draft.
      Subject s;
      s.embargo_until = spec.embargo_until;
      for (const auto& text : spec.analyses) {
        const auto uuid = Uuid::parse(text);
        if (!uuid) {
          refuse(first, "The analysis \"" + text + "\" is not a uuid.");
          continue;
        }
        s.members.push_back(*uuid);
      }
      if (s.members.size() == spec.analyses.size()) subject = std::move(s);
    }
  }

  if (plan.store && published_already) refuse(Destination::Store, "The release is already published.");

  if (subject && plan.earthbank) {
    auto hidden = store.embargoed_analyses(subject->members, now);
    if (!hidden) return fail(hidden.error());
    if (!subject->members.empty() && hidden->size() == subject->members.size()) {
      refuse(Destination::EarthBank, "No member of the release is free of embargo, so there is nothing to send.");
    } else if (plan.earthbank->mode == EarthBankMode::Upload) {
      const bool public_in_run = plan.store && plan.store->make_public;
      Error error;
      auto reason = upload_refusal(store, *subject, public_in_run, now, &error);
      if (!error.what.empty()) return fail(error);
      if (reason) refuse(Destination::EarthBank, std::move(*reason));
    }
  }
  return out;
}

}  // namespace pychron::publish
