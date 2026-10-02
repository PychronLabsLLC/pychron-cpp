// Test-only reader for the Ar-Ar golden vectors (spec 9.4).
//
// The golden files are produced by tools/reduction_golden/generate.py from
// legacy pychron (spec 9.1); the case format is documented in that script's
// docstring and in spec 9.3. This is a minimal strict JSON parser (RFC 8259:
// objects, arrays, strings with escapes and \u surrogate pairs, numbers,
// true/false/null) plus the comparison helpers the golden tests share. It does
// not reuse libs/experiment's parser: pychron_reduction_tests must not link
// experiment. Numbers are converted with std::strtod; the tests never change
// the C locale, so the decimal separator is '.'.
#pragma once

#include <gtest/gtest.h>

#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef PYCHRON_REDUCTION_GOLDEN_DIR
#error "PYCHRON_REDUCTION_GOLDEN_DIR must be defined (tests/reduction/CMakeLists.txt)"
#endif

namespace pychron::reduction::golden {

struct Json {
  enum class Type : std::uint8_t { Null, Bool, Number, String, Array, Object };
  using Array = std::vector<Json>;
  using Object = std::map<std::string, Json, std::less<>>;  // sorted keys

  Type type = Type::Null;
  bool boolean = false;
  double number = 0.0;
  std::string string;
  Array array;
  Object object;

  bool is_null() const noexcept { return type == Type::Null; }
  bool is_bool() const noexcept { return type == Type::Bool; }
  // True for numbers and for the non-finite strings "nan", "inf", "-inf".
  bool is_number() const noexcept {
    return type == Type::Number ||
           (type == Type::String && (string == "nan" || string == "inf" || string == "-inf"));
  }
  bool is_string() const noexcept { return type == Type::String; }
  bool is_array() const noexcept { return type == Type::Array; }
  bool is_object() const noexcept { return type == Type::Object; }

  // Element count of an array or object, 0 otherwise.
  std::size_t size() const noexcept {
    return type == Type::Array ? array.size() : type == Type::Object ? object.size() : 0;
  }
  bool contains(std::string_view key) const {
    return type == Type::Object && object.find(key) != object.end();
  }
  // Missing keys / indices and wrong types yield a shared null value.
  const Json& operator[](std::string_view key) const {
    if (type == Type::Object) {
      if (auto it = object.find(key); it != object.end()) return it->second;
    }
    return null_value();
  }
  const Json& operator[](std::size_t i) const {
    return type == Type::Array && i < array.size() ? array[i] : null_value();
  }

  // A number, or one of the strings "nan", "inf", "-inf" (spec 9.1). Anything
  // else is a test failure and yields NaN.
  double as_number() const {
    if (type == Type::Number) return number;
    if (type == Type::String) {
      if (string == "nan") return std::nan("");
      if (string == "inf") return HUGE_VAL;
      if (string == "-inf") return -HUGE_VAL;
    }
    ADD_FAILURE() << "golden: expected a number";
    return std::nan("");
  }
  bool as_bool() const {
    if (type != Type::Bool) ADD_FAILURE() << "golden: expected a bool";
    return boolean;
  }
  const std::string& as_string() const {
    if (type != Type::String) ADD_FAILURE() << "golden: expected a string";
    return string;
  }
  const Array& as_array() const {
    if (type != Type::Array) ADD_FAILURE() << "golden: expected an array";
    return array;
  }
  const Object& as_object() const {
    if (type != Type::Object) ADD_FAILURE() << "golden: expected an object";
    return object;
  }

  static const Json& null_value() {
    static const Json kNull;
    return kNull;
  }
};

namespace detail {

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  bool run(Json& out, std::string* error) {
    skip_ws();
    if (!value(out, 0)) {
      if (error) *error = error_ + " at offset " + std::to_string(pos_);
      return false;
    }
    skip_ws();
    if (pos_ != text_.size()) {
      if (error) *error = "trailing characters at offset " + std::to_string(pos_);
      return false;
    }
    return true;
  }

 private:
  static constexpr int kMaxDepth = 256;

  bool fail(std::string message) {
    if (error_.empty()) error_ = std::move(message);
    return false;
  }
  bool at_end() const noexcept { return pos_ >= text_.size(); }
  char peek() const noexcept { return at_end() ? '\0' : text_[pos_]; }
  void skip_ws() noexcept {
    while (!at_end() && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' ||
                         text_[pos_] == '\r')) {
      ++pos_;
    }
  }
  bool literal(std::string_view word) {
    if (text_.substr(pos_, word.size()) != word) return fail("invalid literal");
    pos_ += word.size();
    return true;
  }

  bool value(Json& out, int depth) {
    if (depth > kMaxDepth) return fail("nesting too deep");
    switch (peek()) {
      case '{':
        return object(out, depth);
      case '[':
        return array(out, depth);
      case '"':
        out.type = Json::Type::String;
        return string(out.string);
      case 't':
        out.type = Json::Type::Bool;
        out.boolean = true;
        return literal("true");
      case 'f':
        out.type = Json::Type::Bool;
        out.boolean = false;
        return literal("false");
      case 'n':
        out.type = Json::Type::Null;
        return literal("null");
      default:
        return number(out);
    }
  }

  bool object(Json& out, int depth) {
    out.type = Json::Type::Object;
    ++pos_;  // '{'
    skip_ws();
    if (peek() == '}') {
      ++pos_;
      return true;
    }
    for (;;) {
      skip_ws();
      if (peek() != '"') return fail("expected object key");
      std::string key;
      if (!string(key)) return false;
      skip_ws();
      if (peek() != ':') return fail("expected ':'");
      ++pos_;
      skip_ws();
      Json member;
      if (!value(member, depth + 1)) return false;
      out.object.insert_or_assign(std::move(key), std::move(member));
      skip_ws();
      if (peek() == ',') {
        ++pos_;
        continue;
      }
      if (peek() == '}') {
        ++pos_;
        return true;
      }
      return fail("expected ',' or '}'");
    }
  }

  bool array(Json& out, int depth) {
    out.type = Json::Type::Array;
    ++pos_;  // '['
    skip_ws();
    if (peek() == ']') {
      ++pos_;
      return true;
    }
    for (;;) {
      skip_ws();
      Json element;
      if (!value(element, depth + 1)) return false;
      out.array.push_back(std::move(element));
      skip_ws();
      if (peek() == ',') {
        ++pos_;
        continue;
      }
      if (peek() == ']') {
        ++pos_;
        return true;
      }
      return fail("expected ',' or ']'");
    }
  }

  bool hex4(std::uint32_t& cp) {
    if (pos_ + 4 > text_.size()) return fail("truncated \\u escape");
    cp = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[pos_++];
      cp <<= 4;
      if (c >= '0' && c <= '9') {
        cp |= static_cast<std::uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        cp |= static_cast<std::uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        cp |= static_cast<std::uint32_t>(c - 'A' + 10);
      } else {
        return fail("bad \\u escape");
      }
    }
    return true;
  }

  static void append_utf8(std::string& s, std::uint32_t cp) {
    if (cp < 0x80) {
      s.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool string(std::string& out) {
    ++pos_;  // opening quote
    for (;;) {
      if (at_end()) return fail("unterminated string");
      const char c = text_[pos_++];
      if (c == '"') return true;
      if (static_cast<unsigned char>(c) < 0x20) return fail("control character in string");
      if (c != '\\') {
        out.push_back(c);  // UTF-8 bytes pass through
        continue;
      }
      if (at_end()) return fail("unterminated escape");
      switch (text_[pos_++]) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          std::uint32_t cp = 0;
          if (!hex4(cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            std::uint32_t low = 0;
            if (text_.substr(pos_, 2) != "\\u") return fail("unpaired surrogate");
            pos_ += 2;
            if (!hex4(low)) return false;
            if (low < 0xDC00 || low > 0xDFFF) return fail("unpaired surrogate");
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return fail("unpaired surrogate");
          }
          append_utf8(out, cp);
          break;
        }
        default:
          return fail("bad escape");
      }
    }
  }

  static bool digit(char c) noexcept { return c >= '0' && c <= '9'; }

  bool number(Json& out) {
    const std::size_t start = pos_;
    if (peek() == '-') ++pos_;
    if (peek() == '0') {
      ++pos_;
    } else if (digit(peek())) {
      while (digit(peek())) ++pos_;
    } else {
      return fail("invalid value");
    }
    if (peek() == '.') {
      ++pos_;
      if (!digit(peek())) return fail("expected fraction digits");
      while (digit(peek())) ++pos_;
    }
    if (peek() == 'e' || peek() == 'E') {
      ++pos_;
      if (peek() == '+' || peek() == '-') ++pos_;
      if (!digit(peek())) return fail("expected exponent digits");
      while (digit(peek())) ++pos_;
    }
    if (digit(peek())) return fail("leading zero");
    const std::string token(text_.substr(start, pos_ - start));
    errno = 0;
    char* end = nullptr;
    const double v = std::strtod(token.c_str(), &end);
    if (end != token.c_str() + token.size()) return fail("bad number");
    // ERANGE on underflow still yields the correctly rounded subnormal/zero;
    // overflow is not produced by the generator (allow_nan=False).
    if (errno == ERANGE && std::isinf(v)) return fail("number out of range");
    out.type = Json::Type::Number;
    out.number = v;
    return true;
  }

  std::string_view text_;
  std::size_t pos_ = 0;
  std::string error_;
};

}  // namespace detail

// Parses `text`. On error returns null and sets *error (when given) to a
// non-empty message; on success clears *error.
inline Json parse(std::string_view text, std::string* error) {
  if (error) error->clear();
  Json out;
  std::string message;
  if (!detail::Parser(text).run(out, &message)) {
    if (error) *error = message.empty() ? std::string("parse error") : message;
    return Json{};
  }
  return out;
}

// Loads PYCHRON_REDUCTION_GOLDEN_DIR/<file>. Read or parse errors are test
// failures (ADD_FAILURE) and yield null.
inline Json load(std::string_view file) {
  const std::string path = std::string(PYCHRON_REDUCTION_GOLDEN_DIR) + "/" + std::string(file);
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    ADD_FAILURE() << "golden: cannot open " << path;
    return Json{};
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  std::string error;
  Json doc = parse(buffer.str(), &error);
  if (!error.empty()) ADD_FAILURE() << "golden: " << path << ": " << error;
  return doc;
}

// Spec 4.7 tolerances; a case's "tol" object overrides them.
struct Tol {
  double rtol = 1e-12, rtol_err = 1e-10, atol = 0, atol_err = 0;
};

inline Tol tol_of(const Json& c) {
  Tol t;
  const Json& j = c["tol"];
  if (j.contains("rtol")) t.rtol = j["rtol"].as_number();
  if (j.contains("rtol_err")) t.rtol_err = j["rtol_err"].as_number();
  if (j.contains("atol")) t.atol = j["atol"].as_number();
  if (j.contains("atol_err")) t.atol_err = j["atol_err"].as_number();
  return t;
}

// |got - want| <= atol + rtol * |want|. NaN matches only NaN; an infinity
// matches only the same infinity. Failures are non-fatal and name `what`.
inline void expect_close(double got, double want, double rtol, double atol,
                         std::string_view what) {
  if (std::isnan(want) || std::isinf(want)) {
    const bool ok = std::isnan(want) ? std::isnan(got) : got == want;
    if (!ok) ADD_FAILURE() << what << ": got " << got << ", want " << want;
    return;
  }
  const double diff = std::fabs(got - want);
  const double bound = atol + rtol * std::fabs(want);
  if (!(diff <= bound)) {
    ADD_FAILURE() << what << ": got " << ::testing::PrintToString(got) << ", want "
                  << ::testing::PrintToString(want) << " (|diff| " << diff << " > " << bound
                  << " = atol " << atol << " + rtol " << rtol << " * |want|)";
  }
}

}  // namespace pychron::reduction::golden
