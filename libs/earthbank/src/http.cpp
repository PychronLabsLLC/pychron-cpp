#include "pychron/earthbank/http.hpp"

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "pychron/core/error.hpp"

namespace pychron::earthbank {
namespace {

// RFC 3986, section 2.3.
bool is_unreserved(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
         c == '_' || c == '~';
}

char ascii_lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool same_without_case(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
  }
  return true;
}

}  // namespace

bool request_may_have_arrived(const Error& error) { return error.code != kHttpNotSent; }

std::string url_encode(std::string_view text) {
  static constexpr std::string_view kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) {
    const auto byte = static_cast<unsigned char>(ch);
    if (is_unreserved(byte)) {
      out.push_back(ch);
    } else {
      out.push_back('%');
      out.push_back(kHex[byte >> 4]);
      out.push_back(kHex[byte & 0x0Fu]);
    }
  }
  return out;
}

std::string with_query(std::string_view url,
                       std::initializer_list<std::pair<std::string_view, std::string>> params) {
  std::string out(url);
  // '?' opens the query, '&' goes on with one; a url ending in either needs neither.
  char separator = url.find('?') == std::string_view::npos ? '?' : '&';
  for (const auto& [key, value] : params) {
    if (out.empty() || (out.back() != '?' && out.back() != '&')) out.push_back(separator);
    separator = '&';
    out += url_encode(key);
    out.push_back('=');
    out += url_encode(value);
  }
  return out;
}

std::string url_path(std::string_view url) {
  if (const std::size_t scheme = url.find("://"); scheme != std::string_view::npos) {
    // Past the authority: it ends at the first of these.
    const std::size_t end = url.find_first_of("/?#", scheme + 3);
    url = end == std::string_view::npos ? std::string_view{} : url.substr(end);
  }
  url = url.substr(0, url.find_first_of("?#"));
  return url.empty() ? std::string("/") : std::string(url);
}

std::optional<std::string_view> find_header(const HttpRequest& request, std::string_view name) {
  for (const auto& [key, value] : request.headers) {
    if (same_without_case(key, name)) return std::string_view(value);
  }
  return std::nullopt;
}

std::string request_line(const HttpRequest& request) { return request.method + " " + url_path(request.url); }

}  // namespace pychron::earthbank
