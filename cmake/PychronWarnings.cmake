# pychron_set_warnings(<target>)
# Strict warnings for first-party targets only (third-party deps are untouched).
function(pychron_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    if(PYCHRON_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor
      -Woverloaded-virtual -Wnull-dereference -Wimplicit-fallthrough)
    if(PYCHRON_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()
