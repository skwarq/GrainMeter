// SPDX-License-Identifier: MIT
#ifndef GM_PC_MOCK_THINGSPEAK_H
#define GM_PC_MOCK_THINGSPEAK_H

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace gmpc {

// Local stand-in for the ThingSpeak bulk-write endpoint, following the
// documented behaviour (see README "ThingSpeak"):
//  - POST /channels/<id>/bulk_update.json, JSON body with write_api_key/updates
//  - 200 {"success":true} on success, 401 for a wrong key, 400 for bad JSON
//  - duplicate created_at inside one request rejects the whole request
//  - entries whose created_at already exists in the channel are skipped
// Used in-process by the simulator (mock://) and by gm_receiver (HTTP).
class MockThingSpeak {
 public:
  struct Entry {
    std::string createdAt;
    std::map<std::string, std::string> values;  // field1..8, status
  };

  explicit MockThingSpeak(const std::string& logPath = std::string());

  // Optional: require a specific key per channel (otherwise any non-empty).
  void setChannelKey(uint32_t channel, const std::string& key);

  // Returns HTTP status and fills the response body.
  int handle(const std::string& path, const std::string& body,
             std::string* response);

  std::vector<Entry> entries(uint32_t channel);
  size_t requestCount();
  void clear();

 private:
  bool parse(const std::string& body, std::string* key,
             std::vector<Entry>* updates);

  std::mutex mu_;
  std::string logPath_;
  std::map<uint32_t, std::string> keys_;
  std::map<uint32_t, std::vector<Entry>> channels_;
  size_t requests_;
};

}  // namespace gmpc

#endif
