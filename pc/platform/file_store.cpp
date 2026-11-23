// SPDX-License-Identifier: MIT
#include "platform/file_store.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace gmpc {

FileStore::FileStore(const std::string& path) : path_(path) {}

static std::string unescape(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      ++i;
      out.push_back(s[i] == 'n' ? '\n' : s[i]);
    } else {
      out.push_back(s[i]);
    }
  }
  return out;
}

static std::string escape(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '\\') {
      out += "\\\\";
    } else if (c == '\n') {
      out += "\\n";
    } else {
      out.push_back(c);
    }
  }
  return out;
}

bool FileStore::load() {
  std::lock_guard<std::mutex> lock(mu_);
  values_.clear();
  std::ifstream in(path_);
  if (!in) {
    return !std::filesystem::exists(path_);  // missing file = empty store
  }
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    values_[line.substr(0, eq)] = unescape(line.substr(eq + 1));
  }
  return true;
}

bool FileStore::get(const char* key, char* out, size_t cap) {
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = values_.find(key);
  if (it == values_.end() || it->second.size() + 1 > cap) {
    return false;
  }
  std::snprintf(out, cap, "%s", it->second.c_str());
  return true;
}

bool FileStore::set(const char* key, const char* value) {
  std::lock_guard<std::mutex> lock(mu_);
  values_[key] = value;
  return true;
}

bool FileStore::commit() {
  std::lock_guard<std::mutex> lock(mu_);
  const std::string tmp = path_ + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) {
      return false;
    }
    out << "# GrainMeter PC configuration - contains secrets, do not commit\n";
    for (const auto& kv : values_) {
      out << kv.first << '=' << escape(kv.second) << '\n';
    }
    out.flush();
    if (!out) {
      return false;
    }
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path_, ec);
  return !ec;
}

}  // namespace gmpc
