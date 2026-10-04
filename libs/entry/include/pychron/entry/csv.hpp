#pragma once

// Delimited text in and out (sample and package entry spec, section 6,
// csv.hpp): RFC 4180 quoting, a UTF-8 BOM, CRLF or LF, and a delimiter
// sniffed from the header when none is given. Text pasted from a spreadsheet
// (tab-separated) is read the same way.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"

namespace pychron::entry {

struct CsvTable {
  char delimiter = ',';
  std::vector<std::string> header;
  std::vector<std::vector<std::string>> rows;  // blank lines skipped; short rows padded with ""
  std::vector<int> lines;                      // the 1-based line each row starts on
  std::vector<int> ragged;                     // lines of rows with more or fewer fields than the header
};

// The delimiter among , ; tab | that occurs most often (outside quotes) in
// the first line; ties go to the one earlier in that list; ',' when none does.
char sniff_delimiter(std::string_view text);

// An error for an unterminated quote or an empty header.
Result<CsvTable> read_csv(std::string_view text, std::optional<char> delimiter = std::nullopt);

// One line per row, fields quoted when they hold the delimiter, a quote or a
// line break; lines end with "\n".
std::string write_csv(const std::vector<std::string>& header, const std::vector<std::vector<std::string>>& rows,
                      char delimiter = ',');

}  // namespace pychron::entry
