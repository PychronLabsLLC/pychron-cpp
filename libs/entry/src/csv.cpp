#include "pychron/entry/csv.hpp"

#include <array>

namespace pychron::entry {
namespace {

std::string_view strip_bom(std::string_view text) {
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
      static_cast<unsigned char>(text[2]) == 0xBF)
    text.remove_prefix(3);
  return text;
}

}  // namespace

char sniff_delimiter(std::string_view text) {
  text = strip_bom(text);
  constexpr std::array<char, 4> candidates{',', ';', '\t', '|'};
  std::array<int, 4> counts{};
  bool quoted = false;
  for (char c : text) {
    if (c == '"') quoted = !quoted;
    if (!quoted && (c == '\n' || c == '\r')) break;
    if (quoted) continue;
    for (std::size_t i = 0; i < candidates.size(); ++i)
      if (c == candidates[i]) ++counts[i];
  }
  std::size_t best = 0;
  for (std::size_t i = 1; i < candidates.size(); ++i)
    if (counts[i] > counts[best]) best = i;
  return counts[best] > 0 ? candidates[best] : ',';
}

Result<CsvTable> read_csv(std::string_view text, std::optional<char> delimiter) {
  text = strip_bom(text);
  CsvTable table;
  table.delimiter = delimiter.value_or(sniff_delimiter(text));
  const char d = table.delimiter;

  std::vector<std::vector<std::string>> records;
  std::vector<int> starts;
  std::vector<std::string> record;
  std::string field;
  bool quoted = false, field_started = false, any = false;
  int line = 1, record_line = 1;
  const auto end_field = [&] {
    record.push_back(std::move(field));
    field.clear();
    field_started = false;
  };
  const auto end_record = [&] {
    end_field();
    const bool blank = record.size() == 1 && record[0].empty() && !any;
    if (!blank) {
      records.push_back(std::move(record));
      starts.push_back(record_line);
    }
    record.clear();
    any = false;
  };
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (quoted) {
      if (c == '"') {
        if (i + 1 < text.size() && text[i + 1] == '"') {
          field.push_back('"');
          ++i;
        } else {
          quoted = false;
        }
      } else {
        if (c == '\n') ++line;
        field.push_back(c);
      }
      continue;
    }
    if (c == '"' && !field_started) {
      quoted = true;
      field_started = true;
      any = true;
    } else if (c == d) {
      end_field();
      any = true;
    } else if (c == '\r' || c == '\n') {
      if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
      end_record();
      ++line;
      record_line = line;
    } else {
      field.push_back(c);
      field_started = true;
      any = true;
    }
  }
  if (quoted) return fail(ErrorKind::Config, "line " + std::to_string(record_line) + ": a quoted field is not closed");
  if (!field.empty() || !record.empty() || any) end_record();

  if (records.empty()) return fail(ErrorKind::Config, "no header line");
  table.header = std::move(records.front());
  for (auto& h : table.header) {
    std::size_t b = 0, e = h.size();
    while (b < e && (h[b] == ' ' || h[b] == '\t')) ++b;
    while (e > b && (h[e - 1] == ' ' || h[e - 1] == '\t')) --e;
    h = h.substr(b, e - b);
  }
  for (std::size_t r = 1; r < records.size(); ++r) {
    auto& row = records[r];
    if (row.size() != table.header.size()) table.ragged.push_back(starts[r]);
    if (row.size() < table.header.size()) row.resize(table.header.size());
    table.rows.push_back(std::move(row));
    table.lines.push_back(starts[r]);
  }
  return table;
}

std::string write_csv(const std::vector<std::string>& header, const std::vector<std::vector<std::string>>& rows,
                      char delimiter) {
  std::string out;
  const auto put = [&](const std::vector<std::string>& fields) {
    for (std::size_t i = 0; i < fields.size(); ++i) {
      if (i) out.push_back(delimiter);
      const std::string& f = fields[i];
      if (f.find_first_of(std::string{delimiter, '"', '\n', '\r'}) == std::string::npos) {
        out += f;
        continue;
      }
      out.push_back('"');
      for (char c : f) {
        if (c == '"') out.push_back('"');
        out.push_back(c);
      }
      out.push_back('"');
    }
    out.push_back('\n');
  };
  put(header);
  for (const auto& r : rows) put(r);
  return out;
}

}  // namespace pychron::entry
