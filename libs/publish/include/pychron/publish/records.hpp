#pragma once

// The OGC API - Records crawlable catalogue of a release (data publishing
// design, section 5.4): catalog.json and one record per sample plus one for
// the release, GeoJSON, built from the manifest and the row tables. Field
// names beyond record-core are prefixed "pychron:".

#include <string>

#include "pychron/persistence/ids.hpp"
#include "pychron/publish/manifest.hpp"
#include "pychron/publish/tables.hpp"

namespace pychron::publish {

// The path of a sample's directory under tables/ and figures/, by its row
// in the samples table: file_safe(name), with "-<first 8 of uuid>" when
// another sample of the release has the same safe name.
std::string sample_dir_name(const Table& samples, std::size_t row);

std::string catalog_json(const Manifest& manifest, const ReleaseTables& tables);
// Config when the sample is not in the tables.
Result<std::string> sample_record_json(const Manifest& manifest, const ReleaseTables& tables, const Uuid& sample);
std::string release_record_json(const Manifest& manifest, const ReleaseTables& tables);

}  // namespace pychron::publish
