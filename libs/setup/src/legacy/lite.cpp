#include "lite.hpp"

#include <algorithm>
#include <cctype>

namespace pychron::setup::legacy {

std::string trim(std::string_view s) {
  std::size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return std::string(s.substr(a, b - a));
}

std::vector<std::string> split_list(std::string_view s, char sep) {
  std::vector<std::string> out;
  std::size_t at = 0;
  while (at <= s.size()) {
    const auto next = s.find(sep, at);
    std::string part = trim(s.substr(at, next == std::string_view::npos ? std::string_view::npos : next - at));
    if (!part.empty()) out.push_back(std::move(part));
    if (next == std::string_view::npos) break;
    at = next + 1;
  }
  return out;
}

namespace {

std::string unquote(std::string s) {
  s = trim(s);
  if (s.size() >= 2 && ((s.front() == '\'' && s.back() == '\'') || (s.front() == '"' && s.back() == '"'))) {
    const char q = s.front();
    s = s.substr(1, s.size() - 2);
    if (q == '\'') {  // '' is a quote in single-quoted YAML
      std::string out;
      for (std::size_t i = 0; i < s.size(); ++i) {
        out += s[i];
        if (s[i] == '\'' && i + 1 < s.size() && s[i + 1] == '\'') ++i;
      }
      return out;
    }
  }
  return s;
}

// --- YAML -------------------------------------------------------------------

struct Line {
  int indent = 0;
  std::string text;  // without indent and comment
  int number = 0;
};

// A '#' starts a comment at the start of the content or after whitespace,
// outside quotes.
std::string strip_comment(const std::string& s) {
  char quote = 0;
  for (std::size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (quote) {
      if (c == quote) quote = 0;
      continue;
    }
    if ((c == '\'' || c == '"') && (i == 0 || s[i - 1] == ' ' || s[i - 1] == ':' || s[i - 1] == '-' || s[i - 1] == '['))
      quote = c;
    else if (c == '#' && (i == 0 || std::isspace(static_cast<unsigned char>(s[i - 1])))) return s.substr(0, i);
  }
  return s;
}

std::vector<Line> lines_of(std::string_view text) {
  std::vector<Line> out;
  int number = 0;
  std::size_t at = 0;
  while (at <= text.size()) {
    const auto nl = text.find('\n', at);
    std::string raw(text.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at));
    ++number;
    if (!raw.empty() && raw.back() == '\r') raw.pop_back();
    std::size_t indent = 0;
    while (indent < raw.size() && (raw[indent] == ' ' || raw[indent] == '\t')) ++indent;
    std::string body = strip_comment(raw.substr(indent));
    while (!body.empty() && std::isspace(static_cast<unsigned char>(body.back()))) body.pop_back();
    if (!body.empty() && body != "---" && body != "...") out.push_back({static_cast<int>(indent), body, number});
    if (nl == std::string_view::npos) break;
    at = nl + 1;
  }
  return out;
}

// "key: value" -> key, value (value may be empty); false when not a key line.
bool split_key(const std::string& s, std::string& key, std::string& value) {
  char quote = 0;
  for (std::size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (quote) {
      if (c == quote) quote = 0;
      continue;
    }
    if (i == 0 && (c == '\'' || c == '"')) {
      quote = c;
      continue;
    }
    if (c == ':' && (i + 1 == s.size() || s[i + 1] == ' ' || s[i + 1] == '\t')) {
      key = unquote(s.substr(0, i));
      value = trim(s.substr(i + 1));
      return true;
    }
  }
  return false;
}

YNode scalar_node(const std::string& value) {
  YNode n;
  const std::string v = trim(value);
  if (v.empty() || v == "~" || v == "null") return n;
  if (v.front() == '[' && v.back() == ']') {  // flow sequence of scalars
    n.kind = YNode::Kind::Seq;
    for (auto& part : split_list(std::string_view(v).substr(1, v.size() - 2))) n.seq.push_back(scalar_node(part));
    return n;
  }
  n.kind = YNode::Kind::Scalar;
  n.scalar = unquote(v);
  return n;
}

class YamlParser {
 public:
  YamlParser(std::vector<Line> lines, std::vector<std::string>& skipped) : lines_(std::move(lines)), skipped_(skipped) {}

  YNode document() {
    if (lines_.empty()) return {};
    YNode n = block(lines_[0].indent);
    while (i_ < lines_.size()) {  // lines that fit no structure
      skipped_.push_back("line " + std::to_string(lines_[i_].number) + ": " + lines_[i_].text);
      ++i_;
    }
    return n;
  }

 private:
  // The block whose lines start at column `indent`.
  YNode block(int indent) {
    if (i_ >= lines_.size()) return {};
    if (is_item(lines_[i_].text)) return sequence(indent);
    return mapping(indent);
  }

  static bool is_item(const std::string& t) { return t == "-" || t.starts_with("- "); }

  YNode sequence(int indent) {
    YNode n;
    n.kind = YNode::Kind::Seq;
    while (i_ < lines_.size() && lines_[i_].indent == indent && is_item(lines_[i_].text)) {
      Line& l = lines_[i_];
      std::string rest = l.text.size() > 1 ? trim(l.text.substr(2)) : std::string{};
      const int content = indent + 2 + static_cast<int>(l.text.size() > 1 ? l.text.find_first_not_of(' ', 2) - 2 : 0);
      std::string key, value;
      if (rest.empty()) {
        ++i_;
        n.seq.push_back(i_ < lines_.size() && lines_[i_].indent > indent ? block(lines_[i_].indent) : YNode{});
      } else if (split_key(rest, key, value)) {
        // "- key: value": a mapping whose keys line up after the dash.
        l.text = rest;
        l.indent = content;
        n.seq.push_back(mapping(content));
      } else {
        ++i_;
        n.seq.push_back(scalar_node(rest));
      }
    }
    return n;
  }

  YNode mapping(int indent) {
    YNode n;
    n.kind = YNode::Kind::Map;
    while (i_ < lines_.size() && lines_[i_].indent == indent && !is_item(lines_[i_].text)) {
      const Line& l = lines_[i_];
      std::string key, value;
      if (!split_key(l.text, key, value)) {
        skipped_.push_back("line " + std::to_string(l.number) + ": " + l.text);
        ++i_;
        continue;
      }
      ++i_;
      if (!value.empty()) {
        n.keys.push_back(key);
        n.values.push_back(scalar_node(value));
      } else if (i_ < lines_.size() &&
                 (lines_[i_].indent > indent || (lines_[i_].indent == indent && is_item(lines_[i_].text)))) {
        n.keys.push_back(key);
        n.values.push_back(block(lines_[i_].indent));
      } else {
        n.keys.push_back(key);
        n.values.emplace_back();
      }
    }
    return n;
  }

  std::vector<Line> lines_;
  std::size_t i_ = 0;
  std::vector<std::string>& skipped_;
};

// --- XML --------------------------------------------------------------------

class XmlParser {
 public:
  XmlParser(std::string_view text, std::vector<std::string>& skipped) : s_(text), skipped_(skipped) {}

  XNode document() {
    XNode root;
    root.tag = "#document";
    content(root, "");
    if (root.children.size() == 1) return std::move(root.children.front());
    return root;
  }

 private:
  void content(XNode& parent, const std::string& closing) {
    std::string text;
    while (at_ < s_.size()) {
      if (s_.compare(at_, 4, "<!--") == 0) {
        const auto end = s_.find("-->", at_ + 4);
        at_ = end == std::string_view::npos ? s_.size() : end + 3;
      } else if (s_.compare(at_, 2, "<?") == 0 || s_.compare(at_, 2, "<!") == 0) {
        const auto end = s_.find('>', at_);
        at_ = end == std::string_view::npos ? s_.size() : end + 1;
      } else if (s_.compare(at_, 2, "</") == 0) {
        const auto end = s_.find('>', at_);
        const std::string tag = trim(s_.substr(at_ + 2, (end == std::string_view::npos ? s_.size() : end) - at_ - 2));
        at_ = end == std::string_view::npos ? s_.size() : end + 1;
        if (tag != closing) skipped_.push_back("</" + tag + "> where </" + closing + "> was expected");
        break;
      } else if (s_[at_] == '<') {
        parent.children.push_back(element());
      } else {
        const auto next = s_.find('<', at_);
        // Mixed content: the element's own text is what precedes its first
        // child (legacy reads elem.text); text after a child is not part of it.
        if (parent.children.empty())
          text += std::string(s_.substr(at_, next == std::string_view::npos ? std::string_view::npos : next - at_));
        at_ = next == std::string_view::npos ? s_.size() : next;
      }
    }
    parent.text = trim(text);
  }

  XNode element() {
    XNode n;
    const auto end = s_.find('>', at_);
    if (end == std::string_view::npos) {
      skipped_.emplace_back("unterminated tag at the end of the file");
      at_ = s_.size();
      return n;
    }
    std::string inside(s_.substr(at_ + 1, end - at_ - 1));
    at_ = end + 1;
    const bool self_closing = !inside.empty() && inside.back() == '/';
    if (self_closing) inside.pop_back();
    std::size_t i = 0;
    while (i < inside.size() && !std::isspace(static_cast<unsigned char>(inside[i]))) ++i;
    n.tag = inside.substr(0, i);
    // attributes: name="value" or name='value'
    while (i < inside.size()) {
      while (i < inside.size() && std::isspace(static_cast<unsigned char>(inside[i]))) ++i;
      const auto eq = inside.find('=', i);
      if (eq == std::string::npos) break;
      const std::string name = trim(inside.substr(i, eq - i));
      std::size_t v = eq + 1;
      while (v < inside.size() && std::isspace(static_cast<unsigned char>(inside[v]))) ++v;
      if (v >= inside.size()) break;
      const char q = inside[v];
      if (q == '"' || q == '\'') {
        const auto close = inside.find(q, v + 1);
        n.attrs[name] = inside.substr(v + 1, close == std::string::npos ? std::string::npos : close - v - 1);
        i = close == std::string::npos ? inside.size() : close + 1;
      } else {
        std::size_t e = v;
        while (e < inside.size() && !std::isspace(static_cast<unsigned char>(inside[e]))) ++e;
        n.attrs[name] = inside.substr(v, e - v);
        i = e;
      }
    }
    if (!self_closing) content(n, n.tag);
    return n;
  }

  std::string_view s_;
  std::size_t at_ = 0;
  std::vector<std::string>& skipped_;
};

}  // namespace

const YNode* YNode::get(std::string_view key) const {
  if (kind != Kind::Map) return nullptr;
  for (std::size_t i = 0; i < keys.size(); ++i)
    if (keys[i] == key) return &values[i];
  return nullptr;
}

std::string YNode::text(std::string_view key) const {
  const YNode* v = get(key);
  if (v == nullptr) return {};
  if (v->kind == Kind::Scalar) return v->scalar;
  if (v->kind == Kind::Map) return v->text("name");
  return {};
}

YNode parse_yaml(std::string_view text, std::vector<std::string>& skipped) {
  return YamlParser(lines_of(text), skipped).document();
}

const XNode* XNode::child(std::string_view t) const {
  for (const auto& c : children)
    if (c.tag == t) return &c;
  return nullptr;
}

std::string XNode::child_text(std::string_view t) const {
  const XNode* c = child(t);
  return c ? c->text : std::string{};
}

XNode parse_xml(std::string_view text, std::vector<std::string>& skipped) { return XmlParser(text, skipped).document(); }

Ini parse_ini(std::string_view text) {
  Ini out;
  std::string section;
  for (const auto& l : lines_of(text)) {
    std::string s = trim(l.text);
    if (s.empty() || s.front() == '#' || s.front() == ';') continue;
    if (s.front() == '[' && s.back() == ']') {
      section = trim(s.substr(1, s.size() - 2));
      out[section];
      continue;
    }
    auto eq = s.find('=');
    if (eq == std::string::npos) eq = s.find(':');
    if (eq == std::string::npos) continue;
    std::string key = trim(s.substr(0, eq));
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
    out[section][key] = unquote(s.substr(eq + 1));
  }
  return out;
}

}  // namespace pychron::setup::legacy
