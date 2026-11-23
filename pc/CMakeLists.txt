# PC simulator: shared core + simulated hardware + cpp-httplib portal server.
add_library(gm_pcsim STATIC
  sim/sim_clock.cpp
  sim/sim_probe.cpp
  sim/sim_network.cpp
  sim/mock_thingspeak.cpp
  sim/sim_ota.cpp
  platform/file_store.cpp
  platform/web_server.cpp
  app/pc_device.cpp
)
target_include_directories(gm_pcsim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(gm_pcsim PUBLIC gm_core httplib_header)
target_compile_features(gm_pcsim PUBLIC cxx_std_17)
gm_set_warnings(gm_pcsim)
if(WIN32)
  # cpp-httplib needs the Windows 10 socket API
  target_compile_definitions(gm_pcsim PUBLIC _WIN32_WINNT=0x0A00 NOMINMAX)
endif()

add_executable(grainmeter_pc app/main.cpp)
target_link_libraries(grainmeter_pc PRIVATE gm_pcsim)
target_compile_definitions(grainmeter_pc PRIVATE GM_PORTAL_DIR="${PROJECT_SOURCE_DIR}/portal")
gm_set_warnings(grainmeter_pc)

add_executable(gm_receiver receiver/receiver_main.cpp)
target_link_libraries(gm_receiver PRIVATE gm_pcsim)
gm_set_warnings(gm_receiver)
