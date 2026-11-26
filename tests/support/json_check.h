// SPDX-License-Identifier: MIT
// Minimal strict JSON syntax checker for tests (no values are built).
#ifndef GM_TEST_JSON_CHECK_H
#define GM_TEST_JSON_CHECK_H

#include <cctype>
#include <string>

namespace jsoncheck {

class Parser {
 public:
  explicit Parser(const std::string& s) : s_(s), i_(0) {}
  // Returns true when the whole text is one valid JSON value.
  bool valid() {
    ws();
    if (!value()) return false;
    ws();
    return i_ == s_.size();
  }
  size_t pos() const { return i_; }

 private:
  void ws() {
    while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' ||
                              s_[i_] == '\r' || s_[i_] == '\t'))
      ++i_;
  }
  bool lit(const char* t) {
    size_t n = std::char_traits<char>::length(t);
    if (s_.compare(i_, n, t) != 0) return false;
    i_ += n;
    return true;
  }
  bool string() {
    if (i_ >= s_.size() || s_[i_] != '"') return false;
    for (++i_; i_ < s_.size(); ++i_) {
      unsigned char c = static_cast<unsigned char>(s_[i_]);
      if (c == '"') {
        ++i_;
        return true;
      }
      if (c < 0x20) return false;
      if (c == '\\') {
        if (++i_ >= s_.size()) return false;
        char e = s_[i_];
        if (e == 'u') {
          for (int k = 0; k < 4; ++k)
            if (++i_ >= s_.size() ||
                !isxdigit(static_cast<unsigned char>(s_[i_])))
              return false;
        } else if (std::string("\"\\/bfnrt").find(e) == std::string::npos) {
          return false;
        }
      }
    }
    return false;
  }
  bool number() {
    size_t start = i_;
    if (i_ < s_.size() && s_[i_] == '-') ++i_;
    size_t digits = i_;
    while (i_ < s_.size() && isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
    if (i_ == digits) return false;
    if (i_ < s_.size() && s_[i_] == '.') {
      size_t f = ++i_;
      while (i_ < s_.size() && isdigit(static_cast<unsigned char>(s_[i_])))
        ++i_;
      if (i_ == f) return false;
    }
    if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
      ++i_;
      if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
      size_t e = i_;
      while (i_ < s_.size() && isdigit(static_cast<unsigned char>(s_[i_])))
        ++i_;
      if (i_ == e) return false;
    }
    return i_ > start;
  }
  bool value() {
    ws();
    if (i_ >= s_.size()) return false;
    char c = s_[i_];
    if (c == '{') {
      ++i_;
      ws();
      if (i_ < s_.size() && s_[i_] == '}') return ++i_, true;
      for (;;) {
        ws();
        if (!string()) return false;
        ws();
        if (i_ >= s_.size() || s_[i_++] != ':') return false;
        if (!value()) return false;
        ws();
        if (i_ < s_.size() && s_[i_] == ',') {
          ++i_;
          continue;
        }
        return i_ < s_.size() && s_[i_++] == '}';
      }
    }
    if (c == '[') {
      ++i_;
      ws();
      if (i_ < s_.size() && s_[i_] == ']') return ++i_, true;
      for (;;) {
        if (!value()) return false;
        ws();
        if (i_ < s_.size() && s_[i_] == ',') {
          ++i_;
          continue;
        }
        return i_ < s_.size() && s_[i_++] == ']';
      }
    }
    if (c == '"') return string();
    if (c == 't') return lit("true");
    if (c == 'f') return lit("false");
    if (c == 'n') return lit("null");
    return number();
  }

  const std::string& s_;
  size_t i_;
};

// Empty string when valid, otherwise the error position with context.
inline std::string error(const std::string& s) {
  Parser p(s);
  if (p.valid()) return "";
  size_t at = p.pos();
  size_t from = at > 60 ? at - 60 : 0;
  return "invalid JSON at " + std::to_string(at) + ": ..." +
         s.substr(from, 100);
}

}  // namespace jsoncheck

#endif
