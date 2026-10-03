#pragma once

// LegacyRepoBuilder: writes legacy-shaped history into a GitFixture, for
// tests of the adapters that walk a project repository (project import, the
// command line).
//
//   GitFixture repo; repo.init();
//   LegacyRepoBuilder legacy(repo);
//   auto c = legacy.collect("66052-01E", kUuid, "2018-02-20T00:27:10-07:00");
//   legacy.refit("66052-01E", "Ar40", 12.5, "2018-06-05T14:57:22-06:00");
//   legacy.set_tag("66052-01E", "omit", "2018-06-05T15:16:17-06:00");
//
// The files are the real ones under tests/dvc/fixtures/project/IR1010 (the
// unknown 66052-01E; fixtures/README.md, section 2.1). Only what a test asks
// for is rewritten: the record's uuid and identity (identifier, aliquot,
// increment, from the run id), the path, one intercept value. Everything else
// is the fixture's bytes, so every analysis built here is "Felix", identifier
// as given, extract device "Fusions Diode", repository IR1010, and its blanks
// and IC factors name the same reference analyses in other repositories.
//
// The commit sequence of collect() is the one legacy pychron made
// (fixtures/README.md, section 4):
//   <COLLECTION>                        record, raw data, extraction, and the
//                                       spectrometer file the first time
//   <ISOEVO> default collection fits    intercepts, baselines
//   <BLANKS> preceding bu-FD-F-789      blanks
//   <ICFactor> default                  IC factors
// all with the author and date given.
//
// A run id is "<identifier>-<aliquot><step letters>" with a numeric
// identifier of at least four digits ("66052-01E", "66052-02"); its files sit
// under the first three characters, as legacy pychron placed them.
//
// write() and commit() are there for what the named operations do not cover:
// a garbage file, a file with a bare NaN, a deletion, an unknown path.

#include <cctype>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "fixture_files.hpp"
#include "git_fixture.hpp"
#include "pychron/dvc/legacy_layout.hpp"

namespace pychron::dvc::testing {

class LegacyRepoBuilder {
 public:
  // The run id, uuid and spectrometer file of the fixture analysis.
  static constexpr std::string_view kFixtureRunid = "66052-01E";
  static constexpr std::string_view kFixtureUuid = "15fb3686-4aed-40c1-8987-e73a8a52b434";
  static constexpr std::string_view kSpecSha = "6a9b4615cd24138b6ce541f75240dc4091378bb1";
  static constexpr std::string_view kAuthor = "Ann <ann@example.org>";

  // The commits of one collection, in order.
  struct Collected {
    std::string collection, isoevo, blanks, icfactors;
  };

  explicit LegacyRepoBuilder(GitFixture& repo) : repo_(repo) {}

  // The repository path of a file of `runid`: "660/intercepts/52-01E.inte.json".
  static std::string path(std::string_view runid, FileKind kind) {
    if (runid.size() < 4) throw std::invalid_argument("LegacyRepoBuilder: run id too short");
    const std::string prefix(runid.substr(0, 3)), tail(runid.substr(3));
    switch (kind) {
      case FileKind::Record: return prefix + "/" + tail + ".json";
      case FileKind::Data: return prefix + "/.data/" + tail + ".dat.json";
      case FileKind::Extraction: return prefix + "/extraction/" + tail + ".extr.json";
      case FileKind::Intercepts: return prefix + "/intercepts/" + tail + ".inte.json";
      case FileKind::Baselines: return prefix + "/baselines/" + tail + ".base.json";
      case FileKind::Blanks: return prefix + "/blanks/" + tail + ".blan.json";
      case FileKind::IcFactors: return prefix + "/icfactors/" + tail + ".icfa.json";
      case FileKind::Tags: return prefix + "/tags/" + tail + ".tags.json";
      case FileKind::PeakCenter: return prefix + "/peakcenter/" + tail + ".peak.json";
      default: throw std::invalid_argument("LegacyRepoBuilder: not an analysis file kind");
    }
  }

  // The fixture's file of that kind, byte for byte.
  static std::string fixture_text(FileKind kind) { return fixture(kUnknown + path(kFixtureRunid, kind)); }

  // The record of `runid`: the fixture's with its identity rewritten. An
  // empty `uuid` leaves the record without one.
  static std::string record_text(std::string_view runid, std::string_view uuid) {
    auto record = nlohmann::json::parse(fixture_text(FileKind::Record));
    const Identity identity = parse_runid(runid);
    record["identifier"] = identity.identifier;
    record["aliquot"] = identity.aliquot;
    if (identity.increment < 0)
      record["increment"] = nullptr;
    else
      record["increment"] = identity.increment;
    if (uuid.empty())
      record.erase("uuid");
    else
      record["uuid"] = std::string(uuid);
    return record.dump(4);
  }

  // Writes one file of `runid` into the work tree, uncommitted.
  void write(std::string_view runid, FileKind kind, std::string_view text) { repo_.write(path(runid, kind), text); }

  // Commits whatever is in the work tree.
  std::string commit(std::string_view message, std::string_view date, std::string_view author = kAuthor) {
    return repo_.commit(message, date, author);
  }

  // One analysis as legacy pychron collected it: four commits.
  Collected collect(std::string_view runid, std::string_view uuid, std::string_view date,
                    std::string_view author = kAuthor) {
    Collected out;
    write_collection_files(runid, uuid);
    out.collection = repo_.commit("<COLLECTION>", date, author);
    write(runid, FileKind::Intercepts, fixture_text(FileKind::Intercepts));
    write(runid, FileKind::Baselines, fixture_text(FileKind::Baselines));
    out.isoevo = repo_.commit("<ISOEVO> default collection fits", date, author);
    write(runid, FileKind::Blanks, fixture_text(FileKind::Blanks));
    out.blanks = repo_.commit("<BLANKS> preceding bu-FD-F-789", date, author);
    write(runid, FileKind::IcFactors, fixture_text(FileKind::IcFactors));
    out.icfactors = repo_.commit("<ICFactor> default", date, author);
    return out;
  }

  // A later refit: one commit that sets the intercept value of `isotope`.
  std::string refit(std::string_view runid, std::string_view isotope, double new_value, std::string_view date,
                    std::string_view author = kAuthor) {
    write(runid, FileKind::Intercepts, intercepts_text(isotope, new_value));
    return repo_.commit("<ISOEVO> fits=" + std::string(isotope) + "(Parabolic)", date, author);
  }

  // The fixture's intercepts with one value changed.
  static std::string intercepts_text(std::string_view isotope, double value) {
    auto intercepts = nlohmann::json::parse(fixture_text(FileKind::Intercepts));
    intercepts.at(std::string(isotope))["value"] = value;
    return intercepts.dump(4);
  }

  // An analysis that entered the repository without a collection sequence:
  // one "<IMPORT> initial" commit with every file, or, with `reduced` false,
  // without the blanks and IC factors (a collection that never completes).
  std::string import_without_collection(std::string_view runid, std::string_view uuid, std::string_view date,
                                        bool reduced = true) {
    write_collection_files(runid, uuid);
    write(runid, FileKind::Intercepts, fixture_text(FileKind::Intercepts));
    write(runid, FileKind::Baselines, fixture_text(FileKind::Baselines));
    if (reduced) {
      write(runid, FileKind::Blanks, fixture_text(FileKind::Blanks));
      write(runid, FileKind::IcFactors, fixture_text(FileKind::IcFactors));
    }
    return repo_.commit("<IMPORT> initial", date);
  }

  // A "<TAG>" commit that writes the tags file of `runid`.
  std::string set_tag(std::string_view runid, std::string_view tag, std::string_view date) {
    const nlohmann::json tags{{"name", std::string(tag)}, {"note", ""}, {"subgroup", ""}};
    write(runid, FileKind::Tags, tags.dump(4));
    return repo_.commit("<TAG> " + std::string(tag) + " " + std::string(runid), date);
  }

  // The fixture's interpreted age of identifier 66052 (flat format), at the
  // path legacy pychron gave it; its members are 66052-01A .. and include the
  // fixture analysis.
  static constexpr std::string_view kInterpretedAgePath = "660/ia/52.ia.json";
  std::string add_interpreted_age(std::string_view date) {
    repo_.write(kInterpretedAgePath, fixture("ia/IR1010/660/ia/52.ia.json"));
    return repo_.commit("<IA> added interpreted age 01", date);
  }

  GitFixture& repo() { return repo_; }

 private:
  struct Identity {
    std::string identifier;
    int aliquot = 0;
    int increment = -1;
  };

  // "66052-01E" -> {"66052", 1, 4}; "66052-02" -> {"66052", 2, -1}.
  static Identity parse_runid(std::string_view runid) {
    const auto dash = runid.rfind('-');
    if (dash == std::string_view::npos) throw std::invalid_argument("LegacyRepoBuilder: run id has no '-'");
    Identity identity;
    identity.identifier = std::string(runid.substr(0, dash));
    std::size_t at = dash + 1;
    bool digits = false;
    for (; at < runid.size() && std::isdigit(static_cast<unsigned char>(runid[at])); ++at) {
      identity.aliquot = identity.aliquot * 10 + (runid[at] - '0');
      digits = true;
    }
    if (!digits) throw std::invalid_argument("LegacyRepoBuilder: run id has no aliquot");
    if (at < runid.size()) {
      int step = 0;
      for (; at < runid.size(); ++at) {
        if (runid[at] < 'A' || runid[at] > 'Z') throw std::invalid_argument("LegacyRepoBuilder: bad step letters");
        step = step * 26 + (runid[at] - 'A' + 1);
      }
      identity.increment = step - 1;
    }
    return identity;
  }

  // What the <COLLECTION> commit adds.
  void write_collection_files(std::string_view runid, std::string_view uuid) {
    const std::string spectrometer = std::string(kSpecSha) + ".json";
    if (!std::filesystem::exists(repo_.path() / spectrometer))
      repo_.write(spectrometer, fixture(kUnknown + spectrometer));
    write(runid, FileKind::Record, record_text(runid, uuid));
    write(runid, FileKind::Data, fixture_text(FileKind::Data));
    write(runid, FileKind::Extraction, fixture_text(FileKind::Extraction));
  }

  GitFixture& repo_;
};

}  // namespace pychron::dvc::testing
