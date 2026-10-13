#pragma once

// A vendor workbook template (.xlsx) as the EarthBank exporter needs it: open
// the file, read a row (the field names), write data rows into a sheet, and
// save a copy. Internal to pychron::earthbank; neither library it is built on
// (miniz for the zip, pugixml for the XML) shows in this header.
//
// This is not a spreadsheet library. It knows nothing of formulas, number
// formats, column widths, tables or drawings, and never rewrites the shared
// string table. A workbook is a zip of XML parts; the only part ever altered
// is the XML of a sheet that rows were written to, and that is edited as
// text, not parsed and written out again.
//
// Three things are read beside the rows, because the template says with them
// what a column may hold and the exporter has nobody else to ask: the list a
// column's cells are validated against (list_validations, column_values) and
// the fill colour of a cell (cell_fill), by which the templates mark a column
// as required. They are read only; nothing of them is ever written.
//
// The fidelity guarantee, which is why it exists (the website's importer is a
// black box that has only been shown the vendor's own files):
//
//   - a part that was not written to is, uncompressed, byte for byte the
//     template's (its compressed bytes are copied too, not recompressed), and
//     the parts keep the template's names and order;
//   - in a sheet that was written to, every byte before the first written
//     row's start tag and every byte after the last written row's end tag is
//     the template's, except the value of `<dimension ref>` where the sheet
//     has one and the written area extends it;
//   - the same template and the same rows give the same file, byte for byte:
//     nothing is taken from the wall clock;
//   - a rewritten sheet's zip entry is flagged as the template's was (with or
//     without a data descriptor, its name UTF-8 or not).
//
// The templates keep placeholder rows below their header rows: rows of
// empty, styled cells. A written row fills its placeholder: the row keeps its
// start tag (less `spans`, a hint that would no longer be true), a cell the
// template has keeps its style and gains the value, a cell it lacks is added
// unstyled, and a column given no value is left as the template had it. A row
// the sheet does not store is added as a bare `<row>` of the written cells.
// Text is written as an inline string, a number as the shortest digits that
// read back as the same double, plainly (100000, 0.0001) while its decimal
// exponent is between -5 and 15 and as 1.5e-07 or 1e21 outside. What was
// written is read back from the edited sheet and compared before it is kept:
// a sheet with markup this cannot see past (a tag inside a comment) is
// refused, not written wrongly.
//
// In a workbook's strings _xHHHH_ stands for the character HHHH (Excel,
// openpyxl and POI all read it so). Strings read are decoded; text written
// that happens to hold such a sequence has its underscore escaped (_x005F_),
// so it is read as it was given.
//
// Rows and columns are numbered from 1, as a spreadsheet numbers them.

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "pychron/core/error.hpp"

namespace pychron::earthbank::ooxml {

// ---- The zip --------------------------------------------------------------

// One entry of an archive, in the archive's order.
struct ZipEntry {
  std::string name;
  std::string data;    // uncompressed
  std::string stored;  // as it lies in the archive (deflated, usually)
};

// Limits on what is read, so that a damaged or hostile file costs an error
// and not the machine's memory. The larger template is 0.35 MB, and its
// largest part 2.4 MB uncompressed. A part's size is checked as the archive
// claims it, before anything is allocated for it.
inline constexpr std::size_t kMaxArchiveBytes = std::size_t{256} << 20;
inline constexpr std::size_t kMaxPartBytes = std::size_t{32} << 20;
inline constexpr std::size_t kMaxEntries = 4096;

// Every entry of the archive at `file`. For checking what was written; the
// workbook itself unpacks only the parts it reads.
Result<std::vector<ZipEntry>> read_zip(const std::filesystem::path& file);

// ---- The sheet ------------------------------------------------------------

inline constexpr int kMaxColumn = 16384;  // XFD
inline constexpr int kMaxRow = 1048576;

// "A5" for (1, 5), "AA5" for (27, 5), "XFD1" for (16384, 1). Empty when the
// column or the row is outside a sheet.
std::string cell_reference(int column, int row);

// An empty cell is not written at all, and neither is an empty string.
struct Cell {
  std::variant<std::monostate, double, std::string> value;
};

// A drop-down list: a data validation of type `list` and where its entries
// are. Excel keeps one in the sheet's `dataValidations`, or, when the list is
// on another sheet, in the sheet's extension list (`x14:dataValidation`);
// both are read. `columns` are the columns its cells are in (every column of
// every area of `sqref`, each once, ascending; the rows are not kept: the
// Ar-Ar template validates only its first data row, and means the column).
// The source is one of:
//   Range       cells `first_row` to `last_row` of column `column` of `sheet`,
//               given as such or through a name the workbook defines;
//   Inline      the `entries` themselves (`"Yes,No"`), each trimmed;
//   Unresolved  anything else: a name the workbook does not define, a block
//               of several columns, a formula. Not an error: `formula` says
//               what was written.
struct ListValidation {
  enum class Source { Range, Inline, Unresolved };
  std::vector<int> columns;
  Source source = Source::Unresolved;
  std::string sheet;
  int column = 0;
  int first_row = 0;
  int last_row = 0;
  std::vector<std::string> entries;
  std::string formula;  // as the validation has it
};

class Workbook {
 public:
  // Reads the whole file; the template is not needed again afterwards.
  // Io when it cannot be read, Protocol when it is not a workbook.
  static Result<Workbook> open(const std::filesystem::path& template_file);

  // In the workbook's order (the order of the tabs).
  std::vector<std::string> sheet_names() const;

  // One string per column from A to the last cell of `row` that holds
  // something, empty strings for empty cells in between; none for a row the
  // sheet does not store. A shared string is resolved (the runs of a rich
  // text joined, _xHHHH_ decoded), a number is its stored text.
  Result<std::vector<std::string>> row_values(std::string_view sheet, int row) const;

  // The list validations of a sheet, in the sheet's order. Config for a
  // sheet the workbook has not.
  Result<std::vector<ListValidation>> list_validations(std::string_view sheet) const;

  // The cells of one column from `first_row` to `last_row`, read as
  // row_values reads them: one string per row, empty for a cell that holds
  // nothing and for a row the sheet does not store.
  Result<std::vector<std::string>> column_values(std::string_view sheet, int column, int first_row,
                                                 int last_row) const;

  // The colour a cell is filled with: that of its style's fill when the fill
  // is a solid one, as xl/styles.xml writes it (`FFFAD9D6`: alpha, red,
  // green, blue; a theme or indexed colour as `theme 4 tint 0.5`,
  // `indexed 13`: enough to tell whether two cells are filled alike). Empty
  // for no fill, a pattern, a cell without a style and a cell the sheet does
  // not store. row_fills gives a whole row, one per column from A to the
  // row's last cell. Protocol when the workbook's styles cannot be read.
  Result<std::string> cell_fill(std::string_view sheet, int row, int column) const;
  Result<std::vector<std::string>> row_fills(std::string_view sheet, int row) const;

  // Writes `rows` to consecutive rows from `first_row` (at least 2: row 1 is
  // a header in every sheet), `rows[i][j]` to column j + 1. Refused (Config),
  // and the workbook left as it was, when a row to be written already holds
  // a value (a styled, empty cell is not one), when text holds a character
  // XML 1.0 forbids or is not UTF-8, or when a number is not finite: the
  // error names the sheet and the cell. A row with no value to write is left
  // alone.
  Result<void> write_rows(std::string_view sheet, int first_row, const std::vector<std::vector<Cell>>& rows);

  // The template with the rows written, as a new file at `out` (replacing
  // one that is there). Written beside `out` and renamed: a failed save
  // leaves no file, and an existing one as it was.
  Result<void> save(const std::filesystem::path& out) const;

 private:
  struct Sheet {
    std::string name;
    std::size_t entry = 0;  // index in the archive
  };

  Workbook() = default;
  const Sheet* find_sheet(std::string_view name) const;
  Result<std::string> sheet_xml(const Sheet& sheet) const;
  Result<std::vector<std::string>> style_fills() const;

  std::shared_ptr<const std::string> archive_;     // the template file's bytes
  std::vector<Sheet> sheets_;
  std::vector<std::string> shared_strings_;
  std::map<std::string, std::string, std::less<>> defined_names_;  // name -> what it refers to
  std::optional<std::size_t> styles_entry_;                        // xl/styles.xml, when there is one
  std::map<std::size_t, std::string> rewritten_;   // entry index -> new XML
};

}  // namespace pychron::earthbank::ooxml
