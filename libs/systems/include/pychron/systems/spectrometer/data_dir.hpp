#pragma once

// Spectrometer data directory (spectrometer spec section 7.4):
//
//   spectrometer/
//     spectrometer.toml
//     tables/<name>/<timestamp>.toml, tables/<name>/current
//     profiles/<name>.toml
//     molecular_weights.toml
//
// Plus the two auxiliary file formats the config refers to: field tables and
// molecular weights. Table *math* is FieldTable's job; this only reads the
// file shape so the assembler can validate columns and axes.

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/config/diagnostic.hpp"
#include "pychron/core/error.hpp"
#include "pychron/systems/spectrometer/config.hpp"

namespace pychron::spectrometer::cfg {

// ---- layout ---------------------------------------------------------------

std::filesystem::path config_path(const std::filesystem::path& root);             // spectrometer.toml
std::filesystem::path molecular_weights_path(const std::filesystem::path& root);  // molecular_weights.toml
std::filesystem::path table_dir(const std::filesystem::path& root, std::string_view name);
std::filesystem::path table_current_pointer(const std::filesystem::path& root, std::string_view name);
std::filesystem::path profile_path(const std::filesystem::path& root, std::string_view name);

// Profile names (file stems under profiles/), sorted. Empty when absent.
std::vector<std::string> list_profiles(const std::filesystem::path& root);

// Reads tables/<name>/current (a file name, e.g. "2026-09-30T120000.toml",
// surrounding whitespace ignored) and returns the version file it points at.
Result<std::filesystem::path> resolve_current_table(const std::filesystem::path& root, std::string_view name);

// ---- field table files ----------------------------------------------------

enum class FitKind { Discrete, Linear, Quadratic, Cubic };
std::string_view to_string(FitKind fit) noexcept;

struct TablePoint {
  std::string isotope;
  double mass = 0.0;
  std::map<std::string, double> values;  // detector -> value in `axis` units
};

// `fit = "quadratic"`, `axis = "dac"`, `[[points]] isotope mass <det> = value ...`
struct TableFile {
  std::string name;
  std::string source_file;
  FitKind fit = FitKind::Linear;
  Axis axis = Axis::Dac;
  std::vector<TablePoint> points;

  // Detectors with a value in every point.
  std::vector<std::string> columns() const;
  bool has_column(std::string_view detector) const;
};

struct TableLoadReport {
  std::optional<TableFile> table;
  std::vector<config::Diagnostic> diagnostics;
  bool ok() const noexcept { return table.has_value(); }
};

TableLoadReport load_table_from_string(std::string_view toml, std::string_view file, std::string_view name);
TableLoadReport load_table(const std::filesystem::path& file, std::string_view name);

// ---- molecular weights ----------------------------------------------------

// Isotope/species name -> mass in amu. File form: flat `Ar40 = 39.9623831237`.
using MolecularWeights = std::map<std::string, double, std::less<>>;

// Built-in table for the noble gases, used when the data directory has none.
const MolecularWeights& default_molecular_weights();

struct WeightsLoadReport {
  std::optional<MolecularWeights> weights;
  std::vector<config::Diagnostic> diagnostics;
  bool ok() const noexcept { return weights.has_value(); }
};

// File entries override/extend the defaults.
WeightsLoadReport load_molecular_weights_from_string(std::string_view toml, std::string_view file);

// ---- whole directory ------------------------------------------------------

// Everything the assembler needs, fully validated.
struct SpectrometerData {
  std::filesystem::path root;
  SpectrometerConfig config;
  MolecularWeights weights;
  std::map<std::string, TableFile> tables;  // current version of each referenced table
};

struct DataLoadReport {
  std::optional<SpectrometerData> data;
  std::vector<config::Diagnostic> diagnostics;
  bool ok() const noexcept { return data.has_value(); }
};

// Loads `config_file` (and its *.local.toml), molecular_weights.toml (defaults
// if missing) and the current version of every table the config references,
// all relative to config_file's directory, then runs every assembler rule.
// All problems are collected.
DataLoadReport load_spectrometer_data(const std::filesystem::path& config_file);
Result<SpectrometerData> load_spectrometer(const std::filesystem::path& config_file);

}  // namespace pychron::spectrometer::cfg
