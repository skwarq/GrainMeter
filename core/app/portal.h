// SPDX-License-Identifier: MIT
#ifndef GM_PORTAL_H
#define GM_PORTAL_H

#include <stddef.h>
#include <stdint.h>

#include "app/grainmeter.h"
#include "ota/multipart.h"

namespace gm {

// HTTP-library independent portal API. The PC server (cpp-httplib) and the
// ESP32 server (esp_http_server) translate requests into PortalRequest and
// send PortalReply back; static files come from the shared portal/ folder.
//
// Security: every state-changing endpoint requires the service password in
// the "X-GM-Password" header. A custom header cannot be sent cross-origin
// without a CORS preflight (which is never granted), which also blocks CSRF
// from other web pages. Repeated failures lock writes for a while.
struct PortalRequest {
  const char* method;    // "GET" / "POST"
  const char* path;      // without query string
  const char* password;  // X-GM-Password header or NULL
  const char* body;      // application/x-www-form-urlencoded
  size_t bodyLen;
};

struct PortalReply {
  int status;
  const char* contentType;
  const char* body;
  size_t len;
  bool restartAfter;  // platform restarts after the reply was sent
};

enum { kPortalMaxBody = 2048 };
// One history page: 256 rows x 24 B + header = 6.2 KiB.
enum { kHistoryPageRows = 256 };
// Access points listed by POST /api/wifi/scan (before de-duplication).
enum { kWifiScanMax = 40 };

// How the OTA request body reaches Portal::otaFeed():
//  Raw         - the image itself (application/octet-stream), exact length
//  Multipart   - multipart/form-data, parsed by the shared MultipartReader
//  DecodedPart - file part already extracted by the HTTP library; the
//                content length is only an upper bound
enum class OtaBody {
  Raw,
  Multipart,
  DecodedPart
};  // form bodies (OTA streams separately)

class Portal {
 public:
  explicit Portal(GrainMeter& app)
      : app_(app), useMultipart_(false), authFails_(0), lockUntilS_(0) {}

  // Returns false if the path is not an API endpoint (serve a file).
  bool handle(const PortalRequest& req, PortalReply* reply);

  // OTA upload: POST /api/ota with either
  //  - multipart/form-data (plain HTML file input, any .bin from disk), or
  //  - the raw image as body (application/octet-stream, X-File-Name header).
  bool otaStart(const char* password, size_t contentLength, OtaBody kind,
                const char* contentType, const char* fileName,
                PortalReply* reply);
  bool otaFeed(const uint8_t* data, size_t len);
  void otaAbort(const char* reason);
  void otaFinish(PortalReply* reply);

  bool authorized(const char* password);

 private:
  void reply(PortalReply* r, int status);
  void replyError(PortalReply* r, int status, const char* msg);
  void replyOk(PortalReply* r, const char* msg);

  static bool multipartSink(void* ctx, const uint8_t* data, size_t len);

  GrainMeter& app_;
  MultipartReader multipart_;
  bool useMultipart_;
  unsigned authFails_;
  uint32_t lockUntilS_;
  char out_[16384];
};

}  // namespace gm

#endif
