// SPDX-License-Identifier: MIT
#include "app/shell.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace gm {

static const char kHelp[] =
    "Commands:\n"
    "  status | diag            JSON status / full diagnostics\n"
    "  scan                     Search ROM on the 1-Wire bus\n"
    "  measure [store]          read all sensors (store = append to buffer)\n"
    "  map <ROM> <point> [cm]   assign sensor to point 1..8 (+height in cm)\n"
    "  unmap <ROM>              remove assignment\n"
    "  automap                  assign unmapped sensors to free points\n"
    "  config                   show configuration (no secrets)\n"
    "  set <key> <value>        name, wifi_ssid, wifi_pass, report_url, "
    "ts_channel,\n"
    "                           ts_key, diag_channel, diag_key, measure_s, "
    "upload_s,\n"
    "                           svc_pass, diag_every_h, batt_low_mv,\n"
    "                           batt_crit_mv, update_url, update_check_h,\n"
    "                           static_ip, gateway, netmask, dns,\n"
    "                           log_udp (0/1), log_level (1-4), log_host,\n"
    "                           log_port, lan_wake (0/1), ap_delay_s,\n"
    "                           ap_enabled (0 = no SoftAP/captive portal)\n"
    "  apply k=v&k=v ...        set several fields at once (URL-encoded)\n"
    "  upload                   run one upload window now\n"
    "  service | exit-service   enter / leave service mode (ESP32)\n"
    "  sim <action> [k=v ...]   PC fault injection (see README)\n"
    "  restart\n";

void Shell::print(const char* text) { out_(ctx_, text); }

void Shell::printf(const char* fmt, ...) {
  char line[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  print(line);
}

void Shell::printJson(bool ok) {
  if (!ok) {
    print("{\"error\":\"output too large\"}\n");
    return;
  }
  print(buf_);
  print("\n");
}

static void appendEncoded(char* out, size_t cap, size_t* n, const char* s) {
  for (; *s != '\0' && *n + 4 < cap; ++s) {
    const unsigned char c = static_cast<unsigned char>(*s);
    const bool plain = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                       (c >= 'a' && c <= 'z') || c == '-' || c == '_' ||
                       c == '.';
    if (plain) {
      out[(*n)++] = static_cast<char>(c);
    } else {
      *n += static_cast<size_t>(snprintf(out + *n, cap - *n, "%%%02X", c));
    }
  }
  out[*n] = '\0';
}

bool Shell::platform(const char* action, const char* args) {
  // args: "a b k=v" -> "action=a&arg=b&k=v" (first bare word is the action)
  char body[256];
  size_t n = 0;
  body[0] = '\0';
  char tmp[200];
  snprintf(tmp, sizeof(tmp), "%s", args != nullptr ? args : "");
  bool first = true;
  for (char* tok = strtok(tmp, " "); tok != nullptr;
       tok = strtok(nullptr, " ")) {
    if (n > 0 && n + 1 < sizeof(body)) {
      body[n++] = '&';
    }
    char* eq = strchr(tok, '=');
    if (eq != nullptr) {
      *eq = '\0';
      appendEncoded(body, sizeof(body), &n, tok);
      if (n + 1 < sizeof(body)) body[n++] = '=';
      appendEncoded(body, sizeof(body), &n, eq + 1);
    } else {
      appendEncoded(body, sizeof(body), &n, first ? "action" : "arg");
      if (n + 1 < sizeof(body)) body[n++] = '=';
      appendEncoded(body, sizeof(body), &n, tok);
    }
    first = false;
  }
  body[n] = '\0';
  char msg[128] = "";
  const bool ok =
      app_.ports().sys->platformAction(action, Form(body, n), msg, sizeof(msg));
  printf("%s%s\n",
         ok ? "" : "error: ", msg[0] ? msg : (ok ? "ok" : "not supported"));
  return true;
}

bool Shell::exec(const char* lineIn) {
  char line[kShellMaxLine];
  snprintf(line, sizeof(line), "%s", lineIn);
  size_t len = strlen(line);
  while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                     line[len - 1] == ' ')) {
    line[--len] = '\0';
  }
  char* cmd = line;
  while (*cmd == ' ') ++cmd;
  if (*cmd == '\0') {
    return true;
  }
  char* rest = strchr(cmd, ' ');
  if (rest != nullptr) {
    *rest++ = '\0';
    while (*rest == ' ') ++rest;
  } else {
    rest = cmd + strlen(cmd);
  }
  char err[96] = "";

  if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
    print(kHelp);
  } else if (strcmp(cmd, "status") == 0) {
    JsonWriter w(buf_, sizeof(buf_));
    app_.writeStatus(w);
    printJson(w.ok());
  } else if (strcmp(cmd, "diag") == 0) {
    JsonWriter w(buf_, sizeof(buf_));
    app_.writeDiag(w);
    printJson(w.ok());
  } else if (strcmp(cmd, "scan") == 0) {
    const ScanReport& s = app_.scan();
    printf("presence=%s found=%u rom_crc_errors=%u bus_error=%s\n",
           s.search.presence ? "yes" : "no",
           static_cast<unsigned>(s.search.found),
           static_cast<unsigned>(s.search.crcErrors),
           s.search.busError ? "yes" : "no");
    for (size_t i = 0; i < s.search.found; ++i) {
      char hex[kRomTextLen + 1];
      const Rom& r = s.roms[i];
      printf("  %s  family=0x%02X %-15s %s point=%u\n",
             romToHex(r, hex, sizeof(hex)), r.family(), familyName(r.family()),
             familySupported(r.family()) ? "measured    " : "NOT MEASURED",
             app_.romMap().pointOf(r));
    }
  } else if (strcmp(cmd, "measure") == 0) {
    const MeasureReport& m = app_.measure(strcmp(rest, "store") == 0);
    printf("presence=%s conversion=%ums%s\n", m.presence ? "yes" : "no",
           static_cast<unsigned>(m.conversionMs),
           m.conversionTimeout ? " TIMEOUT" : "");
    for (size_t i = 0; i < m.count; ++i) {
      const SensorReading& r = m.readings[i];
      char hex[kRomTextLen + 1];
      romToHex(r.rom, hex, sizeof(hex));
      if (r.status == ReadStatus::Ok) {
        const int a = r.centi < 0 ? -r.centi : r.centi;
        printf("  point %u  %s  %s%d.%02d C\n", r.point, hex,
               r.centi < 0 ? "-" : "", a / 100, a % 100);
      } else {
        printf("  point %u  %s  %s\n", r.point, hex, readStatusName(r.status));
      }
    }
  } else if (strcmp(cmd, "map") == 0) {
    char rom[40] = "";
    long point = 0;
    long height = -1;
    const int n = sscanf(rest, "%39s %ld %ld", rom, &point, &height);
    if (n < 2) {
      print("usage: map <ROM> <point 1..8> [height_cm]\n");
    } else if (app_.mapAssign(rom, point, height, err, sizeof(err))) {
      print("mapping saved\n");
    } else {
      printf("error: %s\n", err);
    }
  } else if (strcmp(cmd, "unmap") == 0) {
    if (app_.mapRemove(rest, err, sizeof(err))) {
      print("mapping removed\n");
    } else {
      printf("error: %s\n", err);
    }
  } else if (strcmp(cmd, "automap") == 0) {
    printf("%u sensor(s) assigned\n", static_cast<unsigned>(app_.mapAuto()));
  } else if (strcmp(cmd, "config") == 0) {
    JsonWriter w(buf_, sizeof(buf_));
    configWriteJson(w, app_.config());
    printJson(w.ok());
  } else if (strcmp(cmd, "set") == 0) {
    char* value = strchr(rest, ' ');
    if (value == nullptr) {
      print("usage: set <key> <value>\n");
      return true;
    }
    *value++ = '\0';
    char body[300];
    size_t n = 0;
    appendEncoded(body, sizeof(body), &n, rest);
    if (n + 1 < sizeof(body)) body[n++] = '=';
    appendEncoded(body, sizeof(body), &n, value);
    if (app_.applyConfig(Form(body, n), err, sizeof(err))) {
      printf("%s saved\n", rest);  // value intentionally not echoed
    } else {
      printf("error: %s\n", err);
    }
  } else if (strcmp(cmd, "apply") == 0) {
    // apply k1=v1&k2=v2 (URL-encoded): all fields validated together and
    // saved atomically, like the portal form (scripts/provision.py).
    if (app_.applyConfig(Form(rest, strlen(rest)), err, sizeof(err))) {
      print("configuration saved\n");
    } else {
      printf("error: %s\n", err);
    }
  } else if (strcmp(cmd, "upload") == 0) {
    const LastUpload& L = app_.upload();
    printf("%s: %s\n", L.ok ? "ok" : "failed", L.message);
  } else if (strcmp(cmd, "service") == 0) {
    return platform("enter_service", rest);
  } else if (strcmp(cmd, "exit-service") == 0) {
    return platform("exit_service", rest);
  } else if (strcmp(cmd, "sim") == 0) {
    return platform("sim", rest);
  } else if (strcmp(cmd, "restart") == 0) {
    restart_ = true;
    print("restarting\n");
  } else {
    printf("unknown command '%s' (try: help)\n", cmd);
    return false;
  }
  return true;
}

}  // namespace gm
