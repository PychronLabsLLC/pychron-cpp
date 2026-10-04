#pragma once

// Deterministic ids for imported rows (legacy ingestion spec, section 3.2).
// Every id is a UUIDv5 in one namespace over a tagged name, so importing the
// same source twice derives the same ids and writes nothing the second time.

#include <string>
#include <string_view>

#include "pychron/persistence/ids.hpp"
#include "pychron/persistence/import.hpp"

namespace pychron::ingest {

inline constexpr std::string_view kImportNamespace = "6f0e4c1a-9d7b-5c2e-8a41-70796368726e";

// One spelling per source, so a repository named two ways gets one set of
// ids. A URL ("scheme://..." or scp-like "user@host:path") loses trailing
// "/" and ".git" and has its scheme and host lower-cased. Anything else is a
// local path, read and returned as UTF-8 text on every platform: made
// absolute and lexically normal, trailing "/" and ".git" removed (a bare
// repository "IR1010.git" and a url that ends in it are named alike),
// otherwise untouched.
std::string normalize_source_url(std::string_view url);

// `url` is a normalized source url in every function below. The name hashed
// is the tag and the arguments joined with '\n'.
persistence::Uuid source_id(persistence::ImportSourceKind kind, std::string_view url, std::string_view branch);
persistence::Uuid changeset_id(std::string_view url, std::string_view commit);
// The collection changeset of one analysis. A commit that adds several
// analyses yields one collection changeset each, so the analysis is part of
// the name.
persistence::Uuid collection_changeset_id(std::string_view url, std::string_view commit, persistence::Uuid analysis);
persistence::Uuid revision_id(std::string_view url, std::string_view commit, std::string_view path);
// `natural_key`: the key parts joined with '\n' (parents by their own keys).
persistence::Uuid catalog_id(std::string_view table, std::string_view natural_key);
// For a legacy record that carries no uuid.
persistence::Uuid derived_analysis_id(std::string_view url, std::string_view runid);
persistence::Uuid conflict_id(std::string_view url, std::string_view commit, std::string_view path);
// A git tag's bookmark, and the analysis group the bookmark is scoped to.
persistence::Uuid bookmark_id(std::string_view url, std::string_view tag);
persistence::Uuid bookmark_group_id(std::string_view url, std::string_view tag);
// An interpreted age, named by the path of its file in the source.
persistence::Uuid interpreted_age_id(std::string_view url, std::string_view path);

}  // namespace pychron::ingest
