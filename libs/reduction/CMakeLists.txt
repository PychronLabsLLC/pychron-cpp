# pychron::reduction. Sources are globbed so units landing in parallel only add
# files under src/ and include/pychron/reduction/ and never edit this list.
file(GLOB_RECURSE PYCHRON_REDUCTION_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(pychron_reduction STATIC ${PYCHRON_REDUCTION_SOURCES})
add_library(pychron::reduction ALIAS pychron_reduction)

target_include_directories(pychron_reduction PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(pychron_reduction PUBLIC cxx_std_20)
target_link_libraries(pychron_reduction PUBLIC pychron::core)

pychron_set_warnings(pychron_reduction)

# Parity with Python's per-operation rounding (spec 4.7): never fuse a * b + c
# into an FMA. PUBLIC because UFloat's arithmetic (combine, variance) is inline
# in a public header: every consumer must compile it unfused too, or the
# linker may pick a contracted copy (spec 7). MSVC's default /fp:precise does
# not contract across statements and needs nothing here.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(pychron_reduction PUBLIC -ffp-contract=off)
endif()
