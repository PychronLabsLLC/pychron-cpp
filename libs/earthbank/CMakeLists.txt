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
#
# A profile's password is kept by the operating system (secret_store.hpp).
# Each system's store is one file, src/secret_store_<system>.cpp, and only the
# one for this platform is compiled: the keychain on macOS (the Security
# framework), the Credential Manager on Windows, the secret service on other
# systems when libsecret is found. PYCHRON_EARTHBANK_SECRET_STORE names what
# was built in, "memory" when nothing was: passwords then last as long as the
# program. -DPYCHRON_EARTHBANK_SYSTEM_SECRET_STORE=OFF builds with none.
file(GLOB PYCHRON_EARTHBANK_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(FILTER PYCHRON_EARTHBANK_SOURCES EXCLUDE REGEX "/secret_store_(mac|win|linux)\\.cpp$")

option(PYCHRON_EARTHBANK_SYSTEM_SECRET_STORE
       "Keep EarthBank passwords in the operating system's store (keychain, Credential Manager, libsecret)" ON)
set(PYCHRON_EARTHBANK_SECRET_STORE "memory")
set(PYCHRON_EARTHBANK_SECRET_STORE_LIBS "")
set(PYCHRON_EARTHBANK_HAS_SYSTEM_SECRET_STORE 1)
if(PYCHRON_EARTHBANK_SYSTEM_SECRET_STORE)
  if(APPLE)
    set(PYCHRON_EARTHBANK_SECRET_STORE "keychain")
    list(APPEND PYCHRON_EARTHBANK_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/secret_store_mac.cpp")
    set(PYCHRON_EARTHBANK_SECRET_STORE_LIBS "-framework Security" "-framework CoreFoundation")
  elseif(WIN32)
    set(PYCHRON_EARTHBANK_SECRET_STORE "credential-manager")
    list(APPEND PYCHRON_EARTHBANK_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/secret_store_win.cpp")
    set(PYCHRON_EARTHBANK_SECRET_STORE_LIBS advapi32)
  else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(PYCHRON_LIBSECRET QUIET IMPORTED_TARGET libsecret-1)
    endif()
    if(PYCHRON_LIBSECRET_FOUND)
      set(PYCHRON_EARTHBANK_SECRET_STORE "libsecret")
      list(APPEND PYCHRON_EARTHBANK_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/secret_store_linux.cpp")
      set(PYCHRON_EARTHBANK_SECRET_STORE_LIBS PkgConfig::PYCHRON_LIBSECRET)
    endif()
  endif()
endif()
if(PYCHRON_EARTHBANK_SECRET_STORE STREQUAL "memory")
  set(PYCHRON_EARTHBANK_HAS_SYSTEM_SECRET_STORE 0)
endif()
message(STATUS "pychron: EarthBank passwords are kept in: ${PYCHRON_EARTHBANK_SECRET_STORE}")

add_library(pychron_earthbank STATIC ${PYCHRON_EARTHBANK_SOURCES})
add_library(pychron::earthbank ALIAS pychron_earthbank)

target_include_directories(pychron_earthbank PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_compile_features(pychron_earthbank PUBLIC cxx_std_20)
target_link_libraries(pychron_earthbank PUBLIC pychron::processing pychron::reduction pychron::core
                                            nlohmann_json::nlohmann_json
                                     PRIVATE miniz::miniz pugixml::pugixml
                                             ${PYCHRON_EARTHBANK_SECRET_STORE_LIBS})
pychron_set_warnings(pychron_earthbank)
target_compile_definitions(pychron_earthbank PRIVATE
  PYCHRON_EARTHBANK_SOURCE_TEMPLATE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/templates"
  PYCHRON_EARTHBANK_SECRET_STORE="${PYCHRON_EARTHBANK_SECRET_STORE}"
  PYCHRON_EARTHBANK_HAS_SYSTEM_SECRET_STORE=${PYCHRON_EARTHBANK_HAS_SYSTEM_SECRET_STORE})
