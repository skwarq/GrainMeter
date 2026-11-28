# SPDX-License-Identifier: MIT
idf_component_register(
  SRCS "ow_slave.cpp" "slave_bus.cpp" "config_store.cpp" "dns_redirect.cpp"
       "web_portal.cpp" "real_sensor.cpp" "main.cpp"
  INCLUDE_DIRS "."
  REQUIRES esp_driver_gpio esp_wifi esp_netif esp_event esp_http_server
           nvs_flash esp_timer esp_hw_support lwip
  EMBED_FILES "index.html")
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
target_compile_definitions(${COMPONENT_LIB} PRIVATE OW_SLAVE_EMBEDDED=1)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror
  -fno-exceptions -fno-rtti -fno-jump-tables -fno-tree-switch-conversion)
