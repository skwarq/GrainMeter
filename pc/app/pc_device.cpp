// SPDX-License-Identifier: MIT
#include "app/pc_device.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>

#include "util/log.h"

namespace gmpc {

static const char* kTag = "pc";

PcDevice::PcDevice(const Options& opt)
    : opt_(opt),
      clock_(opt.speed, opt.manualClock),
      probe_(clock_, opt.sensors),
      net_(clock_),
      mock_(opt.dataDir + "/reports.jsonl"),
      http_(mock_, opt.dataDir + "/updates"),
      ota_(opt.dataDir),
      store_(opt.configPath.empty() ? opt.dataDir + "/grainmeter-pc.conf"
                                    : opt.configPath),
      history_(opt.dataDir + "/history.bin"),
      restartPending_(false),
      wakeReason_("power-on"),
      resetReason_("power-on") {
  std::memset(&retained_, 0xA5, sizeof(retained_));  // RTC RAM after power-up
  clock_.setSynced(opt.wallClockAtStart);
}

static std::string randomPassword() {
  static const char kChars[] =
      "abcdefghijkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  std::random_device rd;
  std::uniform_int_distribution<size_t> dist(0, sizeof(kChars) - 2);
  std::string s;
  for (int i = 0; i < 12; ++i) s.push_back(kChars[dist(rd)]);
  return s;
}

void PcDevice::provision() {
  // Fill a fresh PC configuration so the whole flow works out of the box.
  // Only the local mock receives these values.
  char v[80];
  const bool fresh = !store_.get("wifi_ssid", v, sizeof(v));
  if (fresh && opt_.provisionDefaults) {
    store_.set("wifi_ssid", "SimulatedWiFi");
    store_.set("wifi_pass", "simulated-wifi-pass");
    store_.set("report_url", "mock://thingspeak");
    store_.set("ts_channel", "1000001");
    store_.set("ts_key", "SIMULATEDKEY0001");
    store_.set("diag_channel", "1000002");
    store_.set("diag_key", "SIMULATEDKEY0002");
    store_.set("measure_s", "900");
    store_.set("upload_s", "3600");
  }
  if (!opt_.servicePassword.empty()) {
    store_.set("svc_pass", opt_.servicePassword.c_str());
  } else if (!store_.get("svc_pass", v, sizeof(v)) || v[0] == '\0') {
    store_.set("svc_pass", randomPassword().c_str());
    GM_LOGI(kTag, "generated service password, stored as svc_pass in %s",
            store_.path().c_str());
  }
  store_.commit();
}

bool PcDevice::start(std::string* error) {
  std::error_code ec;
  std::filesystem::create_directories(opt_.dataDir, ec);
  if (ec) {
    *error = "cannot create data dir " + opt_.dataDir;
    return false;
  }
  if (!store_.load()) {
    *error = "cannot read " + store_.path();
    return false;
  }
  provision();
  gm::Ports ports;
  ports.bus = &probe_;
  ports.clock = &clock_;
  ports.store = &store_;
  ports.net = &net_;
  ports.http = &http_;
  ports.sys = this;
  ports.ota = &ota_;
  ports.history = &history_;
  gm::ConfigLimits limits;
  limits.minMeasureS = 5;
  limits.allowInsecureUrl = true;
  app_.reset(
      new gm::GrainMeter(ports, &retained_, limits, "mock://thingspeak"));
  portal_.reset(new gm::Portal(*app_));
  std::lock_guard<std::mutex> lock(mu_);
  app_->begin();
  return true;
}

uint32_t PcDevice::tick() {
  if (restartPending_) {
    restart("software restart");
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (app_->ota().busy()) {
    return 1;  // never "sleep" while an update is being written
  }
  app_->runWifiJoin();
  return app_->runDue(true);
}

void PcDevice::restart(const char* reason) {
  std::lock_guard<std::mutex> lock(mu_);
  restartPending_ = false;
  const std::string pendingVersion =
      app_->ota().state() == gm::OtaSession::State::Done
          ? app_->ota().image().version
          : "";
  const bool newImage = ota_.restart(pendingVersion);
  resetReason_ = reason;
  wakeReason_ = newImage ? "software restart (OTA)" : reason;
  GM_LOGI(kTag, "simulated restart: %s, running %s", reason,
          ota_.lastBooted().c_str());
  store_.load();
  app_.reset(new gm::GrainMeter(app_->ports(), &retained_,
                                gm::ConfigLimits{5, true},
                                "mock://thingspeak"));
  portal_.reset(new gm::Portal(*app_));
  app_->begin();
}

static void copyReply(gm::PortalReply* reply, std::string* body) {
  body->assign(reply->body, reply->len);
  reply->body = body->c_str();
}

bool PcDevice::handle(const gm::PortalRequest& req, gm::PortalReply* reply,
                      std::string* bodyCopy) {
  std::lock_guard<std::mutex> lock(mu_);
  const bool handled = portal_->handle(req, reply);
  if (handled) {
    copyReply(reply, bodyCopy);  // portal buffer is reused by the next call
    if (reply->restartAfter) {
      restartPending_ = true;
    }
  }
  return handled;
}

bool PcDevice::otaStart(const char* password, size_t len, gm::OtaBody kind,
                        const char* name, gm::PortalReply* reply,
                        std::string* body) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!portal_->otaStart(password, len, kind, nullptr, name, reply)) {
    copyReply(reply, body);
    return false;
  }
  return true;
}

bool PcDevice::otaFeed(const uint8_t* data, size_t len) {
  std::lock_guard<std::mutex> lock(mu_);
  return portal_->otaFeed(data, len);
}

void PcDevice::otaAbort(const char* reason) {
  std::lock_guard<std::mutex> lock(mu_);
  portal_->otaAbort(reason);
}

void PcDevice::otaFinish(gm::PortalReply* reply, std::string* body) {
  std::lock_guard<std::mutex> lock(mu_);
  portal_->otaFinish(reply);
  copyReply(reply, body);
  if (reply->restartAfter) {
    restartPending_ = true;
  }
}

static void appendText(void* ctx, const char* text) {
  static_cast<std::string*>(ctx)->append(text);
}

std::string PcDevice::shell(const std::string& line, bool* restart) {
  std::string out;
  std::lock_guard<std::mutex> lock(mu_);
  gm::Shell sh(*app_, appendText, &out);
  sh.exec(line.c_str());
  *restart = sh.restartRequested();
  return out;
}

int PcDevice::batteryMv() {
  if (batteryOverrideMv_ > 0) {
    return batteryOverrideMv_;
  }
  // Li-ion with a solar panel: charges by day, drains by night.
  const double hours = static_cast<double>(clock_.monoMs()) / 3600000.0;
  return static_cast<int>(3850.0 + 250.0 * std::sin(hours / 24.0 * 6.2831853));
}

void PcDevice::writePlatformJson(gm::JsonWriter& w, bool detailed) {
  w.key("sim").beginObject();
  w.kv("speed", static_cast<int64_t>(clock_.speed()))
      .kvb("network_up", net_.up())
      .kvb("lost_response", http_.lostResponse())
      .kvu("http_posts", http_.postCount())
      .kvu("mock_entries_main", mock_.entries(app_->config().channelId).size())
      .kvu("mock_entries_diag",
           mock_.entries(app_->config().diagChannelId).size())
      .kv("config_file", store_.path().c_str());
  w.key("ota_slots")
      .beginObject()
      .kv("running", ota_.runningSlot())
      .kvb("pending_new_image", ota_.pending())
      .kv("last_boot", ota_.lastBooted().c_str())
      .kv("rejected", ota_.rejectedVersion().c_str())
      .kvb("fail_next_boot", ota_.failNextBoot())
      .endObject();
  w.kv("battery_override_mv", static_cast<int64_t>(batteryOverrideMv_.load()))
      .kvu("service_requests", serviceRequests_.load())
      .kvu("time_syncs", net_.syncCount())
      .kvu("http_sessions", http_.sessionCount())
      .kvb("tls", pcTlsEnabled());
  probe_.writeJson(w);
  (void)detailed;
  w.endObject();
}

bool PcDevice::platformAction(const char* action, const gm::Form& args,
                              char* msg, size_t cap) {
  if (std::strcmp(action, "sim") == 0) {
    return simAction(args, msg, cap);
  }
  if (std::strcmp(action, "enter_service") == 0) {
    // Remote service request (manifest): the PC portal is always on, only
    // record it so tests and the UI can see it.
    long minutes = 0;
    args.getInt("minutes", 1, 60, &minutes);
    ++serviceRequests_;
    std::snprintf(msg, cap,
                  "service window of %ld min requested (PC: portal always on)",
                  minutes);
    return true;
  }
  if (std::strcmp(action, "exit_service") == 0) {
    std::snprintf(
        msg, cap,
        "PC simulator: portal is always available (no SoftAP/deep sleep)");
    return false;
  }
  std::snprintf(msg, cap, "unknown action");
  return false;
}

// Called with mu_ held (from the portal or the shell).
bool PcDevice::simAction(const gm::Form& f, char* msg, size_t cap) {
  char action[24] = "";
  char arg[24] = "";
  char value[24] = "";
  long point = 0;
  f.get("action", action, sizeof(action));
  f.get("arg", arg, sizeof(arg));
  f.get("value", value, sizeof(value));
  f.getInt("sensor", 1, 16, &point);  // index on the simulated probe
  const bool on = std::strcmp(arg, "on") == 0 || std::strcmp(arg, "down") == 0;

  if (std::strcmp(action, "crc") == 0) {
    long count = 1;
    f.getInt("count", -1, 1000, &count);
    if (!probe_.injectCrc(static_cast<int>(point), static_cast<int>(count))) {
      std::snprintf(msg, cap, "usage: crc sensor=N [count=K|-1]");
      return false;
    }
    std::snprintf(msg, cap, "sensor %ld: next %ld read(s) corrupted", point,
                  count);
  } else if (std::strcmp(action, "remove") == 0 ||
             std::strcmp(action, "restore") == 0) {
    const bool present = std::strcmp(action, "restore") == 0;
    if (!probe_.setPresent(static_cast<int>(point), present)) {
      std::snprintf(msg, cap, "usage: %s sensor=N", action);
      return false;
    }
    std::snprintf(msg, cap, "sensor %ld %s", point,
                  present ? "restored" : "disconnected");
  } else if (std::strcmp(action, "temp") == 0) {
    char* end = nullptr;
    const double t = std::strtod(value, &end);
    if (value[0] == '\0' || *end != '\0' ||
        !probe_.setTemperature(static_cast<int>(point), t)) {
      std::snprintf(msg, cap, "usage: temp sensor=N value=-55..125");
      return false;
    }
    std::snprintf(msg, cap, "sensor %ld fixed at %s C", point, value);
  } else if (std::strcmp(action, "unsupported") == 0) {
    probe_.setUnsupportedDevice(on);
    std::snprintf(msg, cap, "DS18S20 (0x10) on bus: %s", on ? "yes" : "no");
  } else if (std::strcmp(action, "battery") == 0) {
    long mv = -1;
    if (!f.getInt("value", 0, 5000, &mv)) {
      std::snprintf(msg, cap,
                    "usage: battery value=<mV> (0 = simulated curve)");
      return false;
    }
    batteryOverrideMv_ = static_cast<int>(mv);
    std::snprintf(msg, cap, "battery %s",
                  mv == 0 ? "follows the simulated curve" : value);
  } else if (std::strcmp(action, "ota_fail_boot") == 0) {
    ota_.setFailNextBoot(on);
    std::snprintf(msg, cap, "next new image fails its first start: %s",
                  on ? "yes" : "no");
  } else if (std::strcmp(action, "short") == 0) {
    probe_.setShorted(on);
    std::snprintf(msg, cap, "DATA line shorted: %s", on ? "yes" : "no");
  } else if (std::strcmp(action, "net") == 0) {
    net_.setUp(!on);
    std::snprintf(msg, cap, "network %s", on ? "down (outage)" : "up");
  } else if (std::strcmp(action, "lost_response") == 0) {
    http_.setLostResponse(on);
    std::snprintf(msg, cap, "lost responses: %s", on ? "on" : "off");
  } else if (std::strcmp(action, "speed") == 0) {
    const double s = std::atof(value);
    if (s <= 0) {
      std::snprintf(msg, cap, "usage: speed value=X");
      return false;
    }
    clock_.setSpeed(s);
    std::snprintf(msg, cap, "speed x%s", value);
  } else if (std::strcmp(action, "advance") == 0) {
    long seconds = 0;
    if (!f.getInt("seconds", 1, 30L * 24 * 3600, &seconds)) {
      std::snprintf(msg, cap, "usage: advance seconds=N");
      return false;
    }
    clock_.advanceMs(static_cast<uint64_t>(seconds) * 1000u);
    std::snprintf(msg, cap, "clock advanced by %ld s", seconds);
  } else if (std::strcmp(action, "powerloss") == 0) {
    std::memset(&retained_, 0xA5, sizeof(retained_));  // RTC RAM lost
    clock_.powerCycle();
    resetReason_ = "power-on (simulated power loss)";
    wakeReason_ = "power-on";
    app_->begin();
    std::snprintf(msg, cap,
                  "power loss simulated: buffer lost, state re-initialised");
  } else {
    std::snprintf(msg, cap,
                  "actions: crc, remove, restore, temp, unsupported, short, "
                  "net, lost_response, speed, "
                  "advance, powerloss, battery, ota_fail_boot");
    return false;
  }
  GM_LOGI(kTag, "sim: %s", msg);
  return true;
}

}  // namespace gmpc
