# Install layout and packages (installation wizard spec section 3.7).
#
#   Linux, Windows             macOS (with BUILD_UI)
#   <prefix>/bin/elctl         Pychron.app/Contents/MacOS/Pychron   (pychron-ui)
#   <prefix>/bin/pychron-ui    Pychron.app/Contents/MacOS/elctl
#   <prefix>/share/pychron/    Pychron.app/Contents/Resources/
#     profiles/ examples/        profiles/ examples/
#     python/  (optional)        python/  (optional)
#
# setup::find_resources() and the scripting host look for these next to the
# running program, so an install is relocatable. Everything pychron installs
# is in the "pychron" component (the Qt runtime copy in "pychron_qt"); the
# bundled dependencies' own install rules (headers, static libraries) are left
# out of packages.
#
#   cmake --install <build> --component pychron --prefix <dir>
#   cpack --config <build>/CPackConfig.cmake          (DEB+TGZ, DragNDrop, NSIS+ZIP)
#
# PYCHRON_BUNDLE_PYTHON: a relocatable CPython (python-build-standalone
# "install_only") to ship as python/, for scripted extractions. Configure with
# Python_ROOT_DIR pointing at the same directory so the programs link its
# libpython; they find it through their RPATH (Linux, macOS) or beside them
# (Windows). Without it the programs use the Python they were built against.
#
# PYCHRON_DEPLOY_QT copies the Qt libraries and plugins in (macdeployqt,
# windeployqt; Qt >= 6.5 on Linux). On by default on macOS and Windows; off on
# Linux, where the .deb depends on the distribution's Qt.

include(GNUInstallDirs)

set(PYCHRON_BUNDLE_PYTHON "" CACHE PATH "Relocatable CPython (python-build-standalone install_only) to ship with the programs")
if(APPLE OR WIN32)
  set(_pychron_deploy_qt_default ON)
else()
  set(_pychron_deploy_qt_default OFF)
endif()
option(PYCHRON_DEPLOY_QT "Copy the Qt runtime into the install (macdeployqt / windeployqt)" ${_pychron_deploy_qt_default})

set(PYCHRON_COMPONENT pychron)
if(APPLE AND TARGET pychron-ui)
  set(PYCHRON_INSTALL_BUNDLE "Pychron.app")
  set(PYCHRON_INSTALL_BIN "${PYCHRON_INSTALL_BUNDLE}/Contents/MacOS")
  set(PYCHRON_INSTALL_RESOURCES "${PYCHRON_INSTALL_BUNDLE}/Contents/Resources")
  set(PYCHRON_INSTALL_DOC "${PYCHRON_INSTALL_RESOURCES}")
  set(_pychron_python_rpath "@executable_path/../Resources/python/lib")
else()
  set(PYCHRON_INSTALL_BIN "${CMAKE_INSTALL_BINDIR}")
  set(PYCHRON_INSTALL_RESOURCES "${CMAKE_INSTALL_DATADIR}/pychron")
  set(PYCHRON_INSTALL_DOC "${CMAKE_INSTALL_DATADIR}/doc/pychron")
  if(APPLE)
    set(_pychron_python_rpath "@executable_path/../share/pychron/python/lib")
  else()
    set(_pychron_python_rpath "$ORIGIN/../share/pychron/python/lib")
  endif()
endif()

# --- programs -----------------------------------------------------------------

set(_pychron_programs)
if(TARGET elctl)
  list(APPEND _pychron_programs elctl)
  install(TARGETS elctl RUNTIME DESTINATION "${PYCHRON_INSTALL_BIN}" COMPONENT ${PYCHRON_COMPONENT})
endif()
if(TARGET pychron-ui)
  list(APPEND _pychron_programs pychron-ui)
  if(APPLE)
    set_target_properties(pychron-ui PROPERTIES
      MACOSX_BUNDLE ON
      OUTPUT_NAME Pychron
      MACOSX_BUNDLE_BUNDLE_NAME Pychron
      MACOSX_BUNDLE_GUI_IDENTIFIER com.pychronlabs.pychron
      MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
      MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
      MACOSX_BUNDLE_ICON_FILE pychron.icns)
    # The icon files are the application icon rendered by tools/make_icons.py.
    set(_pychron_icns "${PROJECT_SOURCE_DIR}/packaging/icons/pychron.icns")
    # TARGET_DIRECTORY: source properties belong to a directory, and the target
    # is made in apps/pychron-ui, not here; without it the icon is not copied.
    set_source_files_properties("${_pychron_icns}" TARGET_DIRECTORY pychron-ui
      PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(pychron-ui PRIVATE "${_pychron_icns}")
  elseif(WIN32)
    set_target_properties(pychron-ui PROPERTIES WIN32_EXECUTABLE ON)
    target_sources(pychron-ui PRIVATE "${PROJECT_SOURCE_DIR}/packaging/windows/pychron.rc")
  endif()
  install(TARGETS pychron-ui
    BUNDLE DESTINATION . COMPONENT ${PYCHRON_COMPONENT}
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT ${PYCHRON_COMPONENT})
endif()

# --- shared files ---------------------------------------------------------------

install(DIRECTORY "${PROJECT_SOURCE_DIR}/profiles/" DESTINATION "${PYCHRON_INSTALL_RESOURCES}/profiles"
  COMPONENT ${PYCHRON_COMPONENT})
install(DIRECTORY "${PROJECT_SOURCE_DIR}/configs/examples/" DESTINATION "${PYCHRON_INSTALL_RESOURCES}/examples"
  COMPONENT ${PYCHRON_COMPONENT})
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" "${PROJECT_SOURCE_DIR}/README.md" DESTINATION "${PYCHRON_INSTALL_DOC}"
  COMPONENT ${PYCHRON_COMPONENT})

if(UNIX AND NOT APPLE AND TARGET pychron-ui)
  install(FILES "${PROJECT_SOURCE_DIR}/packaging/linux/pychron.desktop"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/applications" COMPONENT ${PYCHRON_COMPONENT})
  install(FILES "${PROJECT_SOURCE_DIR}/packaging/icons/pychron.png"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps" COMPONENT ${PYCHRON_COMPONENT})
endif()

# --- bundled Python -------------------------------------------------------------

if(PYCHRON_BUNDLE_PYTHON)
  if(NOT EXISTS "${PYCHRON_BUNDLE_PYTHON}")
    message(FATAL_ERROR "PYCHRON_BUNDLE_PYTHON: ${PYCHRON_BUNDLE_PYTHON} does not exist")
  endif()
  # The embedded interpreter needs libpython and the standard library only:
  # no python executable, headers, Tcl/Tk, pip or the developer tools.
  install(DIRECTORY "${PYCHRON_BUNDLE_PYTHON}/" DESTINATION "${PYCHRON_INSTALL_RESOURCES}/python"
    USE_SOURCE_PERMISSIONS COMPONENT ${PYCHRON_COMPONENT}
    REGEX "/(bin|include|share|Scripts|tcl|libs)$" EXCLUDE
    REGEX "/lib/(tcl|tk|itcl|thread|pkgconfig)[^/]*$" EXCLUDE
    REGEX "/lib/libt(cl|k)[^/]*$" EXCLUDE
    REGEX "/lib/libpython3\\.(so|dylib)$" EXCLUDE  # the stable-ABI stub: unused, and its $ORIGIN link breaks dpkg-shlibdeps
    REGEX "/(__pycache__|test|tests|idlelib|tkinter|turtledemo|ensurepip|lib2to3|pydoc_data|venv)$" EXCLUDE
    REGEX "/site-packages/[^/]+$" EXCLUDE
    REGEX "/lib-dynload/_tkinter[^/]*$" EXCLUDE
    REGEX "/config-[^/]*$" EXCLUDE
    REGEX "\\.(exe|pdb|lib)$" EXCLUDE)
  if(NOT WIN32 AND CMAKE_STRIP)
    # python-build-standalone ships its libraries with debug info (~200 MB);
    # strip it from the installed copies (-S: debug symbols only).
    install(CODE "
      file(GLOB_RECURSE _libs
        \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${PYCHRON_INSTALL_RESOURCES}/python/lib/*.so*\"
        \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${PYCHRON_INSTALL_RESOURCES}/python/lib/*.dylib\")
      foreach(_lib IN LISTS _libs)
        if(NOT IS_SYMLINK \"\${_lib}\")
          execute_process(COMMAND \"${CMAKE_STRIP}\" -S \"\${_lib}\")
          if(APPLE)  # a changed library needs signing again (ad hoc) to load
            execute_process(COMMAND codesign --force --sign - \"\${_lib}\")
          endif()
        endif()
      endforeach()" COMPONENT ${PYCHRON_COMPONENT})
  endif()
  if(WIN32)
    # The DLL loader looks beside the program.
    file(GLOB _pychron_python_dlls "${PYCHRON_BUNDLE_PYTHON}/python3*.dll")
    install(FILES ${_pychron_python_dlls} DESTINATION "${PYCHRON_INSTALL_BIN}" COMPONENT ${PYCHRON_COMPONENT})
  else()
    foreach(program IN LISTS _pychron_programs)
      set_property(TARGET ${program} APPEND PROPERTY INSTALL_RPATH "${_pychron_python_rpath}")
    endforeach()
  endif()
endif()

# --- Qt runtime -----------------------------------------------------------------

if(PYCHRON_DEPLOY_QT AND TARGET pychron-ui)
  if(UNIX AND NOT APPLE AND Qt6_VERSION VERSION_LESS 6.5)
    message(WARNING "PYCHRON_DEPLOY_QT: Qt ${Qt6_VERSION} cannot deploy on Linux (needs 6.5); skipped")
  elseif(COMMAND qt6_generate_deploy_script)
    # pychron-ui and elctl share one copy of Qt (elctl uses QtSql for the store).
    if(APPLE)
      set(_pychron_ui_exe "\${QT_DEPLOY_PREFIX}/${PYCHRON_INSTALL_BIN}/Pychron")
    else()
      set(_pychron_ui_exe "\${QT_DEPLOY_PREFIX}/${PYCHRON_INSTALL_BIN}/$<TARGET_FILE_NAME:pychron-ui>")
    endif()
    set(_pychron_extra "")
    if(TARGET elctl)
      set(_pychron_extra "ADDITIONAL_EXECUTABLES \"\${QT_DEPLOY_PREFIX}/${PYCHRON_INSTALL_BIN}/$<TARGET_FILE_NAME:elctl>\"")
    endif()
    # qt6_, not qt_: the versionless name is a macro that forwards ${ARGV}, so
    # CMake expands ${QT_DEPLOY_PREFIX} at configure time (to nothing) and the
    # deploy tool is handed "/Pychron.app".
    qt6_generate_deploy_script(
      TARGET pychron-ui
      OUTPUT_SCRIPT _pychron_deploy_script
      CONTENT "
qt_deploy_runtime_dependencies(
  EXECUTABLE \"${_pychron_ui_exe}\"
  ${_pychron_extra}
  GENERATE_QT_CONF
)")
    # Its own component: the install test (tests/setup) skips it.
    install(SCRIPT "${_pychron_deploy_script}" COMPONENT pychron_qt)
  else()
    message(WARNING "PYCHRON_DEPLOY_QT: this Qt has no qt6_generate_deploy_script (Qt >= 6.5); skipped")
  endif()
endif()

include(PychronPackaging)
