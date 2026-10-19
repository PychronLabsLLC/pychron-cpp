// A scene written as a one-page PDF 1.4 (data publishing design, section 5.6).

#include "pychron/processing/scene_pdf.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <map>
#include <set>

#include "pychron/core/user_file.hpp"

namespace pychron::processing {

namespace {

enum class Font { Regular, Bold, Symbol };

const char* font_name(Font f) {
  switch (f) {
    case Font::Bold:
      return "/F2";
    case Font::Symbol:
      return "/F3";
    case Font::Regular:
      break;
  }
  return "/F1";
}

// A colour channel or an alpha as a fraction, three decimals.
std::string unit(std::uint8_t v) {
  std::array<char, 16> buf{};
  const auto r = std::to_chars(buf.data(), buf.data() + buf.size(), v / 255.0, std::chars_format::fixed, 3);
  std::string s(buf.data(), r.ptr);
  while (s.back() == '0') s.pop_back();
  if (s.back() == '.') s.pop_back();
  return s;
}

std::string rgb(const Color& c) { return unit(c.r) + " " + unit(c.g) + " " + unit(c.b); }

// Code points of WinAnsiEncoding above 0x7e that are not Latin-1's own byte.
std::optional<unsigned char> win_ansi(char32_t c) {
  if (c >= 0x20 && c <= 0x7e) return static_cast<unsigned char>(c);
  if (c >= 0xa0 && c <= 0xff) return static_cast<unsigned char>(c);
  static const std::map<char32_t, unsigned char> kHigh = {
      {U'€', 0x80}, {U'‚', 0x82}, {U'ƒ', 0x83}, {U'„', 0x84}, {U'…', 0x85},
      {U'†', 0x86}, {U'‡', 0x87}, {U'ˆ', 0x88}, {U'‰', 0x89}, {U'Š', 0x8a},
      {U'‹', 0x8b}, {U'Œ', 0x8c}, {U'Ž', 0x8e}, {U'‘', 0x91}, {U'’', 0x92},
      {U'“', 0x93}, {U'”', 0x94}, {U'•', 0x95}, {U'–', 0x96}, {U'—', 0x97},
      {U'˜', 0x98}, {U'™', 0x99}, {U'š', 0x9a}, {U'›', 0x9b}, {U'œ', 0x9c},
      {U'ž', 0x9e}, {U'Ÿ', 0x9f}};
  if (const auto it = kHigh.find(c); it != kHigh.end()) return it->second;
  return std::nullopt;
}

// Greek letters in the Symbol font's encoding.
std::optional<unsigned char> symbol(char32_t c) {
  static constexpr std::string_view kLower = "abgdezhqiklmnxoprVstufcyw";  // U+03B1 .. U+03C9
  static constexpr std::string_view kUpper = "ABGDEZHQIKLMNXOPR?STUFCYW";  // U+0391 .. U+03A9
  if (c >= 0x3b1 && c <= 0x3c9) return static_cast<unsigned char>(kLower[c - 0x3b1]);
  if (c >= 0x391 && c <= 0x3a9 && c != 0x3a2) return static_cast<unsigned char>(kUpper[c - 0x391]);
  return std::nullopt;
}

// A byte of a PDF string literal.
void put_byte(std::string& out, unsigned char b) {
  if (b == '(' || b == ')' || b == '\\') {
    out.push_back('\\');
    out.push_back(static_cast<char>(b));
  } else if (b >= 0x20 && b < 0x7f) {
    out.push_back(static_cast<char>(b));
  } else {
    const std::array<char, 4> oct = {'\\', static_cast<char>('0' + ((b >> 6) & 7)), static_cast<char>('0' + ((b >> 3) & 7)),
                                     static_cast<char>('0' + (b & 7))};
    out.append(oct.data(), oct.size());
  }
}

// UTF-8 text as runs of one font each: WinAnsi in the text's font, Greek in
// Symbol, anything else '?'.
std::vector<std::pair<Font, std::string>> runs(std::string_view text, Font base) {
  std::vector<std::pair<Font, std::string>> out;
  for (std::size_t i = 0; i < text.size();) {
    const char32_t c = next_code_point(text, i);
    if (c < 0x20) continue;  // control characters are not drawn
    Font f = base;
    unsigned char b = '?';
    if (const auto w = win_ansi(c)) {
      b = *w;
    } else if (const auto g = symbol(c)) {
      f = Font::Symbol;
      b = *g;
    }
    if (out.empty() || out.back().first != f) out.emplace_back(f, std::string());
    put_byte(out.back().second, b);
  }
  return out;
}

// The page's graphics states, one per fill alpha (/Fa<n>) and stroke alpha (/Sa<n>).
struct States {
  std::set<int> fill, stroke;
};

class Content {
 public:
  Content(double height, States& states) : height_(height), states_(states) {}

  std::string take() { return std::move(out_); }

  void path(const DrawPath& p, const std::vector<Box>& clips) {
    out_ += "q\n";
    clip(p.clip, clips);
    if (p.fill) {
      alpha(*p.fill, true);
      out_ += rgb(*p.fill) + " rg\n";
    }
    if (p.stroke) {
      alpha(*p.stroke, false);
      out_ += rgb(*p.stroke) + " RG\n" + format_coord(p.width) + " w\n";
      if (const auto dash = dash_pattern(p.dash); !dash.empty()) {
        out_ += '[';
        for (std::size_t i = 0; i < dash.size(); ++i) out_ += (i ? " " : "") + format_coord(dash[i]);
        out_ += "] 0 d\n";
      }
    }
    for (const auto& op : p.ops) {
      switch (op.kind) {
        case PathOp::Kind::Move:
          out_ += pt(op.p[0]) + " m\n";
          break;
        case PathOp::Kind::Line:
          out_ += pt(op.p[0]) + " l\n";
          break;
        case PathOp::Kind::Curve:
          out_ += pt(op.p[0]) + " " + pt(op.p[1]) + " " + pt(op.p[2]) + " c\n";
          break;
        case PathOp::Kind::Close:
          out_ += "h\n";
          break;
      }
    }
    out_ += p.fill && p.stroke ? "B\n" : p.fill ? "f\n" : p.stroke ? "S\n" : "n\n";
    out_ += "Q\n";
  }

  void text(const DrawText& t, const std::vector<Box>& clips) {
    out_ += "q\n";
    clip(t.clip, clips);
    alpha(t.color, true);
    const double w = text_width(t.text, t.size, t.bold);
    const double shift = t.anchor == TextAnchor::Middle ? w / 2 : t.anchor == TextAnchor::End ? w : 0.0;
    const double x = t.vertical ? t.at.x : t.at.x - shift;
    const double y = height_ - t.at.y + (t.vertical ? -shift : 0.0);
    out_ += "BT " + rgb(t.color) + " rg ";
    out_ += t.vertical ? "0 1 -1 0 " : "1 0 0 1 ";
    out_ += format_coord(x) + " " + format_coord(y) + " Tm";
    for (const auto& [font, bytes] : runs(t.text, t.bold ? Font::Bold : Font::Regular))
      out_ += std::string(" ") + font_name(font) + " " + format_coord(t.size) + " Tf (" + bytes + ") Tj";
    out_ += " ET\nQ\n";
  }

  void background(const Color& c, double width) {
    out_ += rgb(c) + " rg\n0 0 " + format_coord(width) + " " + format_coord(height_) + " re\nf\n";
  }

 private:
  std::string pt(Pt p) const { return format_coord(p.x) + " " + format_coord(height_ - p.y); }

  void clip(int index, const std::vector<Box>& clips) {
    if (index < 0 || static_cast<std::size_t>(index) >= clips.size()) return;
    const Box& b = clips[static_cast<std::size_t>(index)];
    out_ += format_coord(b.x) + " " + format_coord(height_ - b.y - b.h) + " " + format_coord(b.w) + " " +
            format_coord(b.h) + " re W n\n";
  }

  void alpha(const Color& c, bool fill) {
    if (c.a == 255) return;
    (fill ? states_.fill : states_.stroke).insert(c.a);
    out_ += std::string(fill ? "/Fa" : "/Sa") + std::to_string(c.a) + " gs\n";
  }

  double height_;
  States& states_;
  std::string out_;
};

// A PDF string literal of plain ASCII text; anything else '?'.
std::string ascii_literal(std::string_view s) {
  std::string out = "(";
  for (std::size_t i = 0; i < s.size();) {
    const char32_t c = next_code_point(s, i);
    put_byte(out, c >= 0x20 && c < 0x7f ? static_cast<unsigned char>(c) : static_cast<unsigned char>('?'));
  }
  return out + ")";
}

std::string pad10(std::size_t v) {
  std::string s = std::to_string(v);
  return std::string(s.size() < 10 ? 10 - s.size() : 0, '0') + s;
}

}  // namespace

std::string draw_list_pdf(const DrawList& list, const PdfInfo& info) {
  States states;
  Content content(list.height, states);
  content.background(list.background, list.width);
  for (const auto& item : list.items) {
    if (const auto* p = std::get_if<DrawPath>(&item))
      content.path(*p, list.clips);
    else
      content.text(std::get<DrawText>(item), list.clips);
  }
  const std::string stream = content.take();

  // Objects 1..8 are fixed; the graphics states follow, fill then stroke.
  std::vector<std::string> objects;
  std::string gs_resources;
  int next = 9;
  std::vector<std::string> gs_objects;
  for (const int a : states.fill) {
    gs_resources += " /Fa" + std::to_string(a) + " " + std::to_string(next++) + " 0 R";
    gs_objects.push_back("<< /Type /ExtGState /ca " + unit(static_cast<std::uint8_t>(a)) + " >>");
  }
  for (const int a : states.stroke) {
    gs_resources += " /Sa" + std::to_string(a) + " " + std::to_string(next++) + " 0 R";
    gs_objects.push_back("<< /Type /ExtGState /CA " + unit(static_cast<std::uint8_t>(a)) + " >>");
  }
  const std::string w = format_coord(list.width), h = format_coord(list.height);
  objects.emplace_back("<< /Type /Catalog /Pages 2 0 R >>");
  objects.emplace_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
  objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + w + " " + h +
                    "] /Resources << /Font << /F1 5 0 R /F2 6 0 R /F3 7 0 R >>" +
                    (gs_resources.empty() ? std::string() : " /ExtGState <<" + gs_resources + " >>") +
                    " >> /Contents 4 0 R >>");
  objects.push_back("<< /Length " + std::to_string(stream.size()) + " >>\nstream\n" + stream + "\nendstream");
  objects.emplace_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>");
  objects.emplace_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding >>");
  objects.emplace_back("<< /Type /Font /Subtype /Type1 /BaseFont /Symbol >>");
  std::string meta = "<< /Producer (pychron-cpp) /CreationDate (D:" +
                     format_utc(static_cast<double>(info.created_unix), "%Y%m%d%H%M%S") + "Z)";
  if (!info.title.empty()) meta += " /Title " + ascii_literal(info.title);
  objects.push_back(meta + " >>");
  for (auto& g : gs_objects) objects.push_back(std::move(g));

  std::string out = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
  std::vector<std::size_t> offsets;
  for (std::size_t i = 0; i < objects.size(); ++i) {
    offsets.push_back(out.size());
    out += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
  }
  const std::size_t xref = out.size();
  out += "xref\n0 " + std::to_string(objects.size() + 1) + "\n0000000000 65535 f \n";
  for (const std::size_t o : offsets) out += pad10(o) + " 00000 n \n";
  out += "trailer\n<< /Size " + std::to_string(objects.size() + 1) + " /Root 1 0 R /Info 8 0 R >>\nstartxref\n" +
         std::to_string(xref) + "\n%%EOF\n";
  return out;
}

std::string scene_pdf(const Scene& scene, const PdfInfo& info, const FigureSize& size) {
  return draw_list_pdf(draw_scene(scene, size), info);
}

Result<void> save_scene_pdf(const Scene& scene, const PdfInfo& info, const std::filesystem::path& path,
                            const FigureSize& size) {
  const std::string bytes = scene_pdf(scene, info, size);
  {
    std::ofstream out(path, std::ios::binary);
    if (!out) return fail(ErrorKind::Io, "cannot write " + path.string());
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) return fail(ErrorKind::Io, "cannot write " + path.string());
  }
  mark_as_user_file(path);
  return {};
}

}  // namespace pychron::processing
