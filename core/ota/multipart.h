// SPDX-License-Identifier: MIT
#ifndef GM_MULTIPART_H
#define GM_MULTIPART_H

#include <stddef.h>
#include <stdint.h>

namespace gm {

// Streaming extractor for the first file part of a multipart/form-data body
// (a plain HTML <input type="file"> form). Data is passed through in chunks
// without buffering the file.
class MultipartReader {
 public:
  // Receives file bytes; return false to stop (e.g. flash write failed).
  typedef bool (*Sink)(void* ctx, const uint8_t* data, size_t len);

  MultipartReader() { reset(); }

  // Parses the boundary from the Content-Type header value.
  bool begin(const char* contentType, Sink sink, void* ctx);
  // Returns false on malformed input or when the sink refused data.
  bool feed(const uint8_t* data, size_t len);

  bool fileComplete() const { return fileDone_; }
  const char* fileName() const { return fileName_; }
  const char* error() const { return error_; }

 private:
  enum class State {
    Delimiter,
    AfterDelimiter,
    Headers,
    FileBody,
    SkipBody,
    Epilogue,
    Failed
  };

  void reset();
  bool emit(const uint8_t* data, size_t len);
  bool flush();
  bool fail(const char* msg);
  bool onHeaders();

  State state_;
  Sink sink_;
  void* ctx_;
  char delim_[80];  // "\r\n--" + boundary
  size_t delimLen_;
  size_t match_;  // delimiter bytes matched so far
  char after_[2];
  size_t afterLen_;
  char headers_[512];
  size_t headersLen_;
  uint8_t out_[512];
  size_t outLen_;
  bool fileDone_;
  bool fileSeen_;
  char fileName_[64];
  const char* error_;
};

}  // namespace gm

#endif
