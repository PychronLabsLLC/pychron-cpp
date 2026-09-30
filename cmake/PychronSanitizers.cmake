# Applies PYCHRON_SANITIZE (e.g. "address,undefined") to every target in the
# build, including third-party ones, so instrumented and uninstrumented code
# never mix.
if(PYCHRON_SANITIZE)
  if(MSVC)
    if(PYCHRON_SANITIZE MATCHES "address")
      add_compile_options(/fsanitize=address)
    endif()
  else()
    add_compile_options(-fsanitize=${PYCHRON_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=${PYCHRON_SANITIZE})
  endif()
  message(STATUS "pychron: sanitizers enabled: ${PYCHRON_SANITIZE}")
endif()
