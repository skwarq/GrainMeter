// SPDX-License-Identifier: MIT
#include "ota/multipart.h"

#include <stdio.h>
#include <string.h>

namespace gm {

void MultipartReader::reset() {
  state_ = State::Failed;
  sink_ = nullptr;
  ctx_ = nullptr;
  delim_[0] = '\0';
  delimLen_ = 0;
  match_ = 0;
  afterLen_ = 0;
  headersLen_ = 0;
  outLen_ = 0;
  fileDone_ = false;
  fileSeen_ = false;
  fileName_[0] = '\0';
  error_ = "not started";
}

bool MultipartReader::fail(const char* msg) {
  state_ = State::Failed;
  error_ = msg;
  return false;
}

bool MultipartReader::begin(const char* contentType, Sink sink, void* ctx) {
  reset();
  sink_ = sink;
  ctx_ = ctx;
  if (contentType == nullptr ||
      strncmp(contentType, "multipart/form-data", 19) != 0) {
    return fail("not multipart/form-data");
  }
  const char* b = strstr(contentType, "boundary=");
  if (b == nullptr) {
    return fail("multipart boundary missing");
  }
  b += 9;
  const bool quoted = *b == '"';
  if (quoted) {
    ++b;
  }
  size_t n = 0;
  while (b[n] != '\0' && b[n] != ';' && b[n] != '"' &&
         !(b[n] == ' ' && !quoted)) {
    ++n;
  }
  if (n == 0 || n > 70) {  // RFC 2046: 1..70 characters
    return fail("invalid multipart boundary");
  }
  delimLen_ = static_cast<size_t>(
      snprintf(delim_, sizeof(delim_), "\r\n--%.*s", static_cast<int>(n), b));
  // The body starts with "--boundary" (no preceding CRLF): pretend the
  // CRLF was already matched.
  match_ = 2;
  state_ = State::Delimiter;
  error_ = nullptr;
  return true;
}

bool MultipartReader::flush() {
  if (outLen_ == 0) {
    return true;
  }
  const size_t n = outLen_;
  outLen_ = 0;
  if (!sink_(ctx_, out_, n)) {
    return fail("upload aborted by receiver");
  }
  return true;
}

bool MultipartReader::emit(const uint8_t* data, size_t len) {
  if (state_ != State::FileBody) {
    return true;  // SkipBody / preamble: discard
  }
  for (size_t i = 0; i < len; ++i) {
    out_[outLen_++] = data[i];
    if (outLen_ == sizeof(out_) && !flush()) {
      return false;
    }
  }
  return true;
}

static bool findParam(const char* headers, const char* name, char* out,
                      size_t cap) {
  const char* p = strstr(headers, name);
  if (p == nullptr) {
    return false;
  }
  p += strlen(name);
  size_t n = 0;
  while (*p != '\0' && *p != '"' && n + 1 < cap) {
    out[n++] = *p++;
  }
  out[n] = '\0';
  return *p == '"';
}

bool MultipartReader::onHeaders() {
  headers_[headersLen_] = '\0';
  char name[64];
  // Only the first part carrying a file name is the firmware; other form
  // fields are skipped.
  if (!fileSeen_ && findParam(headers_, "filename=\"", name, sizeof(name))) {
    snprintf(fileName_, sizeof(fileName_), "%s", name);
    fileSeen_ = true;
    state_ = State::FileBody;
  } else {
    state_ = State::SkipBody;
  }
  return true;
}

bool MultipartReader::feed(const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    const uint8_t c = data[i];
    switch (state_) {
      case State::Failed:
        return false;
      case State::Epilogue:
        return true;  // ignore anything after the closing delimiter
      case State::Headers:
        if (headersLen_ + 1 >= sizeof(headers_)) {
          return fail("multipart part headers too long");
        }
        headers_[headersLen_++] = static_cast<char>(c);
        if (headersLen_ >= 4 &&
            memcmp(headers_ + headersLen_ - 4, "\r\n\r\n", 4) == 0) {
          headersLen_ -= 4;
          onHeaders();
          match_ = 0;
        }
        break;
      case State::AfterDelimiter:
        after_[afterLen_++] = static_cast<char>(c);
        if (afterLen_ == 2) {
          if (after_[0] == '-' && after_[1] == '-') {
            state_ = State::Epilogue;
          } else if (after_[0] == '\r' && after_[1] == '\n') {
            state_ = State::Headers;
            headersLen_ = 0;
          } else {
            return fail("malformed multipart delimiter");
          }
        }
        break;
      case State::Delimiter:
      case State::FileBody:
      case State::SkipBody:
        if (c == static_cast<uint8_t>(delim_[match_])) {
          if (++match_ == delimLen_) {
            if (state_ == State::FileBody) {
              if (!flush()) {
                return false;
              }
              fileDone_ = true;
            }
            state_ = State::AfterDelimiter;
            afterLen_ = 0;
            match_ = 0;
          }
        } else {
          // Not a delimiter: the partially matched bytes were data.
          // '\r' only occurs at delim_[0], so restarting is exact.
          if (match_ > 0 &&
              !emit(reinterpret_cast<const uint8_t*>(delim_), match_)) {
            return false;
          }
          match_ = 0;
          if (c == static_cast<uint8_t>(delim_[0])) {
            match_ = 1;
          } else if (!emit(&c, 1)) {
            return false;
          }
        }
        break;
    }
  }
  return state_ != State::Failed;
}

}  // namespace gm
