// A scene written as SVG (data publishing design, section 5.6).

#include "pychron/processing/scene_svg.hpp"

namespace pychron::processing {

namespace {

// Text and attribute values: markup escaped, control characters (invalid in
// XML 1.0) dropped but for tab.
std::string escape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20 && c != '\t') break;
        out.push_back(c);
    }
  }
  return out;
}

std::string rgb(const Color& c) { return color_hex(Color{c.r, c.g, c.b, 255}); }

std::string opacity(const Color& c) { return format_coord(c.a / 255.0); }

void paint(std::string& out, const char* what, const std::optional<Color>& c) {
  out += ' ';
  out += what;
  if (!c) {
    out += "=\"none\"";
    return;
  }
  out += "=\"" + rgb(*c) + "\"";
  if (c->a != 255) out += std::string(" ") + what + "-opacity=\"" + opacity(*c) + "\"";
}

void clip_attr(std::string& out, int clip) {
  if (clip >= 0) out += " clip-path=\"url(#c" + std::to_string(clip) + ")\"";
}

void path(std::string& out, const DrawPath& p) {
  out += "<path d=\"";
  for (const auto& op : p.ops) {
    switch (op.kind) {
      case PathOp::Kind::Move:
        out += "M" + format_coord(op.p[0].x) + " " + format_coord(op.p[0].y);
        break;
      case PathOp::Kind::Line:
        out += "L" + format_coord(op.p[0].x) + " " + format_coord(op.p[0].y);
        break;
      case PathOp::Kind::Curve:
        out += 'C';
        for (std::size_t i = 0; i < 3; ++i)
          out += (i ? " " : "") + format_coord(op.p[i].x) + " " + format_coord(op.p[i].y);
        break;
      case PathOp::Kind::Close:
        out += 'Z';
        break;
    }
  }
  out += '"';
  paint(out, "fill", p.fill);
  if (p.stroke) {
    paint(out, "stroke", p.stroke);
    out += " stroke-width=\"" + format_coord(p.width) + "\"";
    if (const auto dash = dash_pattern(p.dash); !dash.empty()) {
      out += " stroke-dasharray=\"";
      for (std::size_t i = 0; i < dash.size(); ++i) out += (i ? " " : "") + format_coord(dash[i]);
      out += '"';
    }
  } else {
    out += " stroke=\"none\"";
  }
  clip_attr(out, p.clip);
  out += "/>\n";
}

void text(std::string& out, const DrawText& t) {
  const std::string x = format_coord(t.at.x), y = format_coord(t.at.y);
  out += "<text x=\"" + x + "\" y=\"" + y + "\" font-size=\"" + format_coord(t.size) + "\"";
  if (t.bold) out += " font-weight=\"bold\"";
  if (t.anchor == TextAnchor::Middle) out += " text-anchor=\"middle\"";
  if (t.anchor == TextAnchor::End) out += " text-anchor=\"end\"";
  paint(out, "fill", t.color);
  if (t.vertical) out += " transform=\"rotate(-90 " + x + " " + y + ")\"";
  clip_attr(out, t.clip);
  out += ">" + escape(t.text) + "</text>\n";
}

}  // namespace

std::string draw_list_svg(const DrawList& list) {
  const std::string w = format_coord(list.width), h = format_coord(list.height);
  const std::string family =
      (list.font_family.empty() ? std::string() : escape(list.font_family) + ", ") + "Helvetica, Arial, sans-serif";
  std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  out += R"(<svg xmlns="http://www.w3.org/2000/svg" width=")" + w + R"(pt" height=")" + h + R"(pt" viewBox="0 0 )" + w +
         " " + h + "\" font-family=\"" + family + "\">\n";
  if (!list.clips.empty()) {
    out += "<defs>\n";
    for (std::size_t i = 0; i < list.clips.size(); ++i) {
      const Box& b = list.clips[i];
      out += "<clipPath id=\"c" + std::to_string(i) + "\"><rect x=\"" + format_coord(b.x) + "\" y=\"" +
             format_coord(b.y) + "\" width=\"" + format_coord(b.w) + "\" height=\"" + format_coord(b.h) +
             "\"/></clipPath>\n";
    }
    out += "</defs>\n";
  }
  out += "<rect width=\"" + w + "\" height=\"" + h + "\"";
  paint(out, "fill", list.background);
  out += "/>\n";
  for (const auto& item : list.items) {
    if (const auto* p = std::get_if<DrawPath>(&item))
      path(out, *p);
    else
      text(out, std::get<DrawText>(item));
  }
  out += "</svg>\n";
  return out;
}

std::string scene_svg(const Scene& scene, const FigureSize& size) { return draw_list_svg(draw_scene(scene, size)); }

}  // namespace pychron::processing
