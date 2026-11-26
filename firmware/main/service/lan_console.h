// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>

// LAN console (protocol v1, PC side: tools/lan_console.py).
//
// The device announces itself with a UDP broadcast on port 5515
//   GM-HELLO v1 ssid=<ap ssid> name=<name> ip=<ip> port=5516 fw=<v>
//   window_ms=<ms>
// and accepts one TCP client on port 5516: banner, "AUTH?", "auth <service
// password>", then the USB console commands plus live log lines, "#ping" ->
// "#pong" heartbeat, "logs on|off", "exit". While a session is open the
// device does not sleep; it ends after 60 s without a received line, after
// 4 h, or on "exit" ("BYE <reason>" is sent first).

struct LanAnnounce {
  const char* ssid;  // SoftAP name, identifies the device
  const char* name;  // configured device name
  const char* ip;    // station address
  const char* fw;
};

// Upload window on battery: hello once, then wait up to windowMs for the
// script to connect. Returns true when a session started. The server is
// closed again when nobody came.
bool lanOffer(const LanAnnounce& a, uint32_t windowMs);

// Service mode loops (non-blocking): keeps the server open and repeats the
// hello every helloEveryMs.
void lanPoll(const LanAnnounce& a, uint32_t helloEveryMs);

// Stops listening (an open session is not affected).
void lanServerClose();

bool lanSessionActive();
// Ends the session from outside ("BYE <reason>").
void lanSessionEnd(const char* reason);

// Restart during a session (OTA, "restart"): remembered in RTC memory so the
// next boot announces itself for a while and the script reconnects.
void lanMarkResume();
bool lanTakeResume();
