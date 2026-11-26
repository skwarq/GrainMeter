// SPDX-License-Identifier: MIT
#ifndef GM_PORTAL_TUNNEL_H
#define GM_PORTAL_TUNNEL_H
#include "app/portal.h"
namespace gm {
struct TunnelOut {
  const char* payload;
  size_t len;
  bool restart;
  int status;
};
class PortalTunnel {
 public:
  enum { kMaxResponse = 20 * 1024, kMaxCache = 14 * 1024 };
  // responseLimit is useful for constrained transports and overflow tests.
  PortalTunnel(Portal& portal, GrainMeter& app,
               size_t responseLimit = kMaxResponse);
  void beginSession(const char* nonceHex32, const char* deviceId);
  static uint32_t kdfIterations() { return 10000; }
  // Call after LAN/password edits (and before status publication). A change
  // invalidates cached envelopes, not consumed sequences or response IVs.
  bool refreshKeys();
  // Empty output means unsigned status. Like handle(), this reuses output_
  // and requires serialized access. It does not derive keys.
  void statusMac(const char* fw, const char* name, uint32_t until,
                 uint32_t remaining, uint32_t budget, const char* opMid,
                 const char* opFw, const char* opResult, char out[65]);
  bool handle(const char* mid, const char* payload, size_t len, bool retained,
              uint32_t nowMonoS, TunnelOut* out);
  // The transport serializes access and sets this while another request is
  // queued/running. This flag is not a mutex; handle() never changes it.
  void setBusy(bool busy) { busy_ = busy; }

 private:
  bool readCache(int client, TunnelOut* out);
  void eraseCache(int client);
  void storeCache(int client, const TunnelOut& out);
  void respond(const char* mid, const char* cid, uint64_t seq, int status,
               const char* ctype, const char* body, size_t len, bool restart,
               const char* key, TunnelOut* out);
  Portal& portal_;
  GrainMeter& app_;
  size_t limit_;
  char nonce_[33], id_[25], password_[65];
  uint8_t macKey_[32], encKey_[32], retired_[128];
  struct Client {
    char cid[17], mid[17], requestMac[65];
    uint64_t seq, age;
  } clients_[4];
  uint64_t age_, ivCounter_;
  int cacheClient_;
  unsigned failures_;
  uint32_t lockStart_;
  bool locked_, busy_, session_;
  // Up to four variable-sized envelopes share this compacted pool. Pressure
  // evicts response entries by client LRU, never replay metadata. Exact retries
  // without an envelope receive duplicate_expired, without last_seq.
  char output_[kMaxResponse], cache_[kMaxCache];
  char requestPath_[257], requestBody_[2733], plaintext_[kPortalMaxBody + 1];
  size_t cacheLen_;
  int cacheStatus_;
  bool cacheRestart_;
};
static_assert(sizeof(PortalTunnel) < 40 * 1024,
              "Tunnel must preserve heap headroom");
}  // namespace gm
#endif
