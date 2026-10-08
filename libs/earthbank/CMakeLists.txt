# pychron::earthbank: publishing Ar-Ar results to EarthBank (AuScope's
# AusGeochem data platform). The service's records and vocabulary tables, and
# the mapping from pychron-cpp's analyses to them. Qt-free and without network
# code. nlohmann_json is PUBLIC: to_json is the records' interface; processing
# and reduction are, for the mapping's options. miniz and pugixml are PRIVATE:
# only src/ooxml.cpp (the workbook templates' zip and XML) sees them. Sources
# are globbed so units landing in parallel only add files.
#
# templates/ holds the vendor's two workbooks, which the exporter (xlsx.hpp)
# copies and fills. An installed program finds them beside itself
# (cmake/PychronInstall.cmake puts them there); one that is not installed
# finds them here, through PYCHRON_EARTHBANK_SOURCE_TEMPLATE_DIR.
file(GLOB PYCHRON_EARTHBANK_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(pychron_earthbank STATIC ${PYCHRON_EARTHBANK_SOURCES})
add_library(pychron::earthbank ALIAS pychron_earthbank)

target_include_directories(pychron_earthbank PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_compile_features(pychron_earthbank PUBLIC cxx_std_20)
target_link_libraries(pychron_earthbank PUBLIC pychron::processing pychron::reduction pychron::core
                                            nlohmann_json::nlohmann_json
                                     PRIVATE miniz::miniz pugixml::pugixml)
pychron_set_warnings(pychron_earthbank)
target_compile_definitions(pychron_earthbank PRIVATE
  PYCHRON_EARTHBANK_SOURCE_TEMPLATE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/templates")
