idf_component_register(
  SRCS
    main.cpp
    hw/onewire_gpio.cpp
    hw/battery.cpp
    hw/board_io.cpp
    platform/esp_clock.cpp
    platform/nvs_store.cpp
    platform/wifi_link.cpp
    platform/https_client.cpp
    platform/esp_ota_target.cpp
    platform/esp_system_port.cpp
    platform/history_partition.cpp
    service/dns_redirect.cpp
    service/web_portal.cpp
    service/usb_console.cpp
    service/udp_log.cpp
  INCLUDE_DIRS .
  REQUIRES gm_core esp_driver_gpio esp_driver_usb_serial_jtag esp_adc esp_wifi esp_netif esp_event
           nvs_flash esp_http_client esp_http_server app_update esp_partition mbedtls lwip esp_timer
           esp_driver_tsens espcoredump esp_ringbuf
  EMBED_FILES ${CMAKE_CURRENT_LIST_DIR}/../../portal/index.html.gz)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wshadow -Werror)
