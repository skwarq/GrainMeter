# Warning flags for code owned by this project (third-party code is exempt).
function(gm_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    # /WX only for the shared core: PC/test glue is not built locally on MSVC.
    if(GM_WERROR AND target STREQUAL "gm_core")
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wcast-align
      -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual
      -Wnull-dereference -Wdouble-promotion -Wimplicit-fallthrough)
    if(GM_WERROR)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

# Shared application logic: C++11, no exceptions, no RTTI (same as ESP-IDF).
function(gm_no_exceptions target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /EHs-c- /GR-)
    target_compile_definitions(${target} PRIVATE _HAS_EXCEPTIONS=0)
  else()
    target_compile_options(${target} PRIVATE -fno-exceptions -fno-rtti)
  endif()
endfunction()
