// SPDX-License-Identifier: MIT
#include "sim/mock_thingspeak.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>

namespace gmpc {

MockThingSpeak::MockThingSpeak(const std::string& logPath)
    : logPath_(logPath), requests_(0) {}

void MockThingSpeak::setChannelKey(uint32_t channel, const std::string& key) {
  std::lock_guard<std::mutex> lock(mu_);
  keys_[channel] = key;
}

namespace {

// Tiny tolerant reader for the flat JSON produced by tsBuildBulk.
class Reader {
 public:
  explicit Reader(const std::string& s) : s_(s), i_(0) {}
  void ws() {
    while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' ||
                              s_[i_] == '\r' || s_[i_] == '\t'))
      ++i_;
  }
  bool eat(char c) {
    ws();
    if (i_ < s_.size() && s_[i_] == c) {
      ++i_;
      return true;
    }
    return false;
  }
  bool peek(char c) {
    ws();
    return i_ < s_.size() && s_[i_] == c;
  }
  bool string(std::string* out) {
    ws();
    if (i_ >= s_.size() || s_[i_] != '"') return false;
    ++i_;
    out->clear();
    while (i_ < s_.size() && s_[i_] != '"') {
      if (s_[i_] == '\\' && i_ + 1 < s_.size()) {
        ++i_;
      }
      out->push_back(s_[i_++]);
    }
    if (i_ >= s_.size()) return false;
    ++i_;
    return true;
  }
  // number, true/false/null as raw text
  bool scalar(std::string* out) {
    ws();
    if (peek('"')) return string(out);
    out->clear();
    while (i_ < s_.size() && s_[i_] != ',' && s_[i_] != '}' && s_[i_] != ']' &&
           s_[i_] != ' ') {
      out->push_back(s_[i_++]);
    }
    return !out->empty();
  }
  bool done() {
    ws();
    return i_ == s_.size();
  }

 private:
  const std::string& s_;
  size_t i_;
};

}  // namespace

bool MockThingSpeak::parse(const std::string& body, std::string* key,
                           std::vector<Entry>* updates) {
  Reader r(body);
  if (!r.eat('{')) return false;
  bool haveUpdates = false;
  while (!r.peek('}')) {
    std::string k;
    if (!r.string(&k) || !r.eat(':')) return false;
    if (k == "write_api_key") {
      if (!r.string(key)) return false;
    } else if (k == "updates") {
      haveUpdates = true;
      if (!r.eat('[')) return false;
      while (!r.peek(']')) {
        if (!r.eat('{')) return false;
        Entry e;
        while (!r.peek('}')) {
          std::string ek, ev;
          if (!r.string(&ek) || !r.eat(':') || !r.scalar(&ev)) return false;
          if (ek == "created_at") {
            e.createdAt = ev;
          } else {
            e.values[ek] = ev;
          }
          if (!r.eat(',')) break;
        }
        if (!r.eat('}')) return false;
        updates->push_back(e);
        if (!r.eat(',')) break;
      }
      if (!r.eat(']')) return false;
    } else {
      std::string ignored;
      if (!r.scalar(&ignored)) return false;
    }
    if (!r.eat(',')) break;
  }
  return r.eat('}') && r.done() && haveUpdates;
}

static std::string errorBody(int status, const char* code, const char* msg) {
  char buf[256];
  std::snprintf(buf, sizeof(buf),
                "{\"status\":\"%d\",\"error\":{\"error_code\":\"%s\","
                "\"message\":\"%s\"}}",
                status, code, msg);
  return buf;
}

int MockThingSpeak::handle(const std::string& path, const std::string& body,
                           std::string* response) {
  std::lock_guard<std::mutex> lock(mu_);
  ++requests_;
  unsigned long channel = 0;
  char tail[64] = "";
  if (std::sscanf(path.c_str(), "/channels/%lu/%63s", &channel, tail) != 2 ||
      std::string(tail) != "bulk_update.json") {
    *response =
        errorBody(404, "error_resource_not_found", "Resource Not Found");
    return 404;
  }
  std::string key;
  std::vector<Entry> updates;
  if (!parse(body, &key, &updates)) {
    *response = errorBody(400, "error_bad_request", "Bad Request");
    return 400;
  }
  const auto expected = keys_.find(static_cast<uint32_t>(channel));
  if (key.empty() || (expected != keys_.end() && expected->second != key)) {
    *response = errorBody(401, "error_auth_required", "Authorization Required");
    return 401;
  }
  if (updates.size() > 960) {
    *response =
        errorBody(413, "error_request_too_large", "Request Entity Too Large");
    return 413;
  }
  std::set<std::string> inRequest;
  for (const Entry& e : updates) {
    if (e.createdAt.empty() || !inRequest.insert(e.createdAt).second) {
      *response = errorBody(400, "error_bad_request",
                            "duplicate or missing created_at");
      return 400;
    }
  }
  std::vector<Entry>& stored = channels_[static_cast<uint32_t>(channel)];
  std::set<std::string> existing;
  for (const Entry& e : stored) existing.insert(e.createdAt);
  std::ofstream log;
  if (!logPath_.empty()) {
    log.open(logPath_, std::ios::app);
  }
  for (const Entry& e : updates) {
    if (existing.count(e.createdAt) != 0) {
      continue;  // timestamp already in channel: rejected, no duplicate
    }
    stored.push_back(e);
    if (log) {
      log << "{\"channel\":" << channel << ",\"created_at\":\"" << e.createdAt
          << "\"";
      for (const auto& kv : e.values) {
        const bool str = kv.first == "status";
        log << ",\"" << kv.first << "\":" << (str ? "\"" : "") << kv.second
            << (str ? "\"" : "");
      }
      log << "}\n";
    }
  }
  *response = "{\"success\":true}";
  return 200;
}

std::vector<MockThingSpeak::Entry> MockThingSpeak::entries(uint32_t channel) {
  std::lock_guard<std::mutex> lock(mu_);
  return channels_[channel];
}

size_t MockThingSpeak::requestCount() {
  std::lock_guard<std::mutex> lock(mu_);
  return requests_;
}

void MockThingSpeak::clear() {
  std::lock_guard<std::mutex> lock(mu_);
  channels_.clear();
  requests_ = 0;
}

}  // namespace gmpc
