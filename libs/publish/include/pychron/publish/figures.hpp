#pragma once

// The figures of a release (data publishing design, section 5.6): per sample
// an ideogram, an age spectrum per step-heated aliquot and an inverse
// isochron, each as SVG and PDF, built from the release's dataset with the
// release's options and written by processing's Qt-free writers.

#include <cstdint>
#include <string>
#include <vector>

#include "pychron/processing/dataset.hpp"
#include "pychron/processing/options.hpp"
#include "pychron/publish/build.hpp"

namespace pychron::publish {

struct FigureFile {
  std::string path;  // relative to the release directory: figures/<sample>/<kind>.svg|.pdf
  std::string bytes;
};

// A figure's options: the schema's defaults, with error_bar_nsigma = sigma
// and, where the schema has them, plateau.method, plateau.nsteps and
// plateau.gas_fraction from the release options. A value the schema refuses
// (out of its range) leaves the default and, given `warnings`, says so.
processing::Options figure_options(const processing::SchemaPtr& schema, const ReleaseOptions& options,
                                   std::vector<std::string>* warnings = nullptr);

// The figures of one sample (its items of the release dataset), SVG then PDF
// of each, in the order ideogram, spectrum, isochron. A spectrum per aliquot
// with steps (increment >= 0), by identifier and aliquot: file spectrum.*
// when there is one, else spectrum-<identifier>-<aliquot, two digits>.*, made
// file-safe, with -2, -3, ... when two names are the same ignoring case. An
// isochron when at least three items are included. The PDFs' CreationDate
// is created_unix. A figure whose builder fails is left out and
// "<sample_dir>: no <kind>: <why>" added to warnings.
std::vector<FigureFile> sample_figures(const processing::Dataset& sample, const std::string& sample_dir,
                                       const ReleaseOptions& options, std::int64_t created_unix,
                                       std::vector<std::string>& warnings);

}  // namespace pychron::publish
