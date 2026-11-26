# GoogleTest suites. GM_VALGRIND=ON runs every test binary under Valgrind
# memcheck (Linux), failing on any error or definite leak.
include(GoogleTest)

option(GM_VALGRIND "Run tests under valgrind memcheck" OFF)

function(gm_add_test_binary name)
  add_executable(${name} ${ARGN})
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(${name} PRIVATE gm_pcsim GTest::gtest_main)
  target_compile_features(${name} PRIVATE cxx_std_17)
  target_compile_definitions(${name} PRIVATE GM_TEST_PORTAL_DIR="${PROJECT_SOURCE_DIR}/portal")
  gm_set_warnings(${name})
  if(GM_VALGRIND)
    find_program(VALGRIND valgrind REQUIRED)
    add_test(NAME ${name}
      COMMAND ${VALGRIND} --tool=memcheck --error-exitcode=99 --leak-check=full
              --errors-for-leak-kinds=definite --show-leak-kinds=definite
              --track-origins=yes $<TARGET_FILE:${name}>)
    set_tests_properties(${name} PROPERTIES TIMEOUT 900)
  else()
    gtest_discover_tests(${name} DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 300)
  endif()
endfunction()

gm_add_test_binary(gm_power_stats_tests
  unit/test_power_stats.cpp
  ${PROJECT_SOURCE_DIR}/firmware/main/platform/power_stats.cpp
)
target_include_directories(gm_power_stats_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/firmware/main
  ${CMAKE_CURRENT_SOURCE_DIR}/support/power_stats
)

gm_add_test_binary(gm_unit_tests
  unit/test_log_buffer.cpp
  unit/test_base64url.cpp
  unit/test_ota_upload.cpp
  unit/test_hmac_sha256.cpp
  unit/test_portal_tunnel.cpp
  unit/test_notifications.cpp
  unit/test_alarm.cpp
  unit/test_onewire.cpp
  unit/test_ds18b20.cpp
  unit/test_rommap.cpp
  unit/test_retained_state.cpp
  unit/test_retained_clock.cpp
  unit/test_thingspeak.cpp
  unit/test_http_date.cpp
  unit/test_battery_forecast.cpp
  unit/test_scheduler.cpp
  unit/test_talkback.cpp
  unit/test_config.cpp
  unit/test_ota.cpp
  unit/test_json_form.cpp
  unit/test_dns_reply.cpp
  unit/test_remote_update.cpp
  unit/test_history_log.cpp
)

gm_add_test_binary(gm_integration_tests
  integration/test_ota_tunnel.cpp
  integration/test_battery_forecast.cpp
  integration/test_upload.cpp
  integration/test_portal.cpp
  integration/test_scenario.cpp
  integration/test_remote_update.cpp
  integration/test_pc_tls.cpp
)
