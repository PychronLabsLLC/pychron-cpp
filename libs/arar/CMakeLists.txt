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

# pychron::arar: figures (ideogram, spectrum, inverse isochron, spectrum +
# isochron), the Schaen et al. (2021) report, group ages, the derived age, the
# flux fit and view, and the Ar-Ar Domain. Links processing; processing never
# links it. Sources are globbed (top level of src/ only: src/reduction/ is
# pychron::arar_reduction's).
file(GLOB PYCHRON_ARAR_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(pychron_arar STATIC ${PYCHRON_ARAR_SOURCES})
add_library(pychron::arar ALIAS pychron_arar)

target_include_directories(pychron_arar
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  # The figures share processing's private figure helpers (figure_common.hpp,
  # schema_builder.hpp) with the neutral figures that stay there.
  PRIVATE ${PROJECT_SOURCE_DIR}/libs/processing/src
)
target_link_libraries(pychron_arar PUBLIC pychron::processing pychron::arar_reduction)
pychron_set_warnings(pychron_arar)

# pychron::arar_store: the flux store (monitor sets, a level's saved fit, flux
# options) over the DVC store; exists only when processing's store adapter does.
if(TARGET pychron_processing_store)
  file(GLOB PYCHRON_ARAR_STORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/adapters/store/src/*.cpp")
  add_library(pychron_arar_store STATIC ${PYCHRON_ARAR_STORE_SOURCES})
  add_library(pychron::arar_store ALIAS pychron_arar_store)
  target_include_directories(pychron_arar_store PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/adapters/store/include>
    $<INSTALL_INTERFACE:include>
  )
  target_link_libraries(pychron_arar_store PUBLIC pychron::arar pychron::processing_store
    PRIVATE nlohmann_json::nlohmann_json)
  pychron_set_warnings(pychron_arar_store)
endif()
