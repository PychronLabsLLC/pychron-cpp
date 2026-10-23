# libs/arar: the Ar-Ar domain (domain seam spec 3.1).
#
# pychron::arar_reduction: the Ar-Ar reduction (types, reduce(), decay, flux
# math, the live-parity bridge). Links pychron::reduction only. Sources are
# globbed so units landing in parallel only add files under src/reduction/.
file(GLOB_RECURSE PYCHRON_ARAR_REDUCTION_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/reduction/*.cpp")

add_library(pychron_arar_reduction STATIC ${PYCHRON_ARAR_REDUCTION_SOURCES})
add_library(pychron::arar_reduction ALIAS pychron_arar_reduction)

target_include_directories(pychron_arar_reduction PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_link_libraries(pychron_arar_reduction PUBLIC pychron::reduction)
pychron_set_warnings(pychron_arar_reduction)

# Parity with Python's per-operation rounding (reduction spec 4.7): never fuse
# a * b + c into an FMA. pychron::reduction already sets it PUBLIC; repeated
# here so this library's own arithmetic does not depend on that. MSVC's
# default /fp:precise does not contract across statements and needs nothing.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(pychron_arar_reduction PUBLIC -ffp-contract=off)
endif()
