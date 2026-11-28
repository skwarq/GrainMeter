# SPDX-License-Identifier: MIT
idf_component_register(SRCS "app_main.cpp" "capture.cpp" "ow_decoder.cpp"
                            "dns_redirect.cpp" "dns_reply.cpp" "web_portal.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_gpio esp_timer esp_wifi esp_netif
                                esp_http_server nvs_flash lwip)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
