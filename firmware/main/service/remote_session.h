// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "app/platform.h"
#include "util/json.h"

// Port methods require DeviceLock, just like Portal::handle().
gm::RemoteSessionPort& remoteSessionPort();

// Called under DeviceLock at the end of an upload window, before Wi-Fi drops.
void remoteAfterUpload();
// Main loop only, outside DeviceLock. Lazy creation and teardown happen here.
void remotePoll();
void remoteResume();
void remoteServiceStart();
void remoteBeforeRestart();
void remoteStop();
// Shared USB/LAN shell extension; caller holds DeviceLock.
bool remoteConsole(const char* line, void (*out)(void*, const char*),
                   void* ctx);
// Caller holds DeviceLock.
void remoteWriteJson(gm::JsonWriter& w);

// Log sender task only, outside DeviceLock and MQTT callbacks. These never
// start/extend a session. Publish pins the client until the call completes.
bool remoteLogOnline();
bool remoteLogPublish(const char* text, size_t len);
