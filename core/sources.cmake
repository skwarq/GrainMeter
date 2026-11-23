# Single list of shared sources, used by core/CMakeLists.txt (PC) and
# firmware/components/gm_core/CMakeLists.txt (ESP-IDF).
set(GM_CORE_DIR ${CMAKE_CURRENT_LIST_DIR})
set(GM_CORE_SOURCES
  ${GM_CORE_DIR}/util/crc.cpp
  ${GM_CORE_DIR}/util/json.cpp
  ${GM_CORE_DIR}/util/form.cpp
  ${GM_CORE_DIR}/util/log.cpp
  ${GM_CORE_DIR}/util/sha256.cpp
  ${GM_CORE_DIR}/util/json_read.cpp
  ${GM_CORE_DIR}/onewire/rom.cpp
  ${GM_CORE_DIR}/onewire/onewire.cpp
  ${GM_CORE_DIR}/onewire/ds18b20.cpp
  ${GM_CORE_DIR}/data/rommap.cpp
  ${GM_CORE_DIR}/data/retained_state.cpp
  ${GM_CORE_DIR}/data/history_log.cpp
  ${GM_CORE_DIR}/comm/thingspeak.cpp
  ${GM_CORE_DIR}/comm/dns_reply.cpp
  ${GM_CORE_DIR}/app/config.cpp
  ${GM_CORE_DIR}/app/scheduler.cpp
  ${GM_CORE_DIR}/app/grainmeter.cpp
  ${GM_CORE_DIR}/app/portal.cpp
  ${GM_CORE_DIR}/app/shell.cpp
  ${GM_CORE_DIR}/ota/ota_image.cpp
  ${GM_CORE_DIR}/ota/ota_session.cpp
  ${GM_CORE_DIR}/ota/multipart.cpp
  ${GM_CORE_DIR}/ota/remote_update.cpp
)
