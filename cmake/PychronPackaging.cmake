# Packages from the "pychron" install component (cmake/PychronInstall.cmake):
#   Linux    .deb (dependencies from dpkg-shlibdeps) and .tar.gz
#   macOS    .dmg: drag Pychron to Applications (signed: packaging/macos/sign_app.sh)
#   Windows  NSIS installer (Start menu, uninstaller, elctl on PATH) and .zip

set(CPACK_PACKAGE_NAME "pychron")
set(CPACK_PACKAGE_VENDOR "Pychron Labs LLC")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Noble-gas mass spectrometry: instrument control, extraction lines and Ar/Ar data reduction")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/PychronLabsLLC/pychron-cpp")
set(CPACK_PACKAGE_CONTACT "Pychron Labs LLC")
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Pychron")
set(CPACK_STRIP_FILES ON)
# Only pychron's own files, never the dependencies' headers and archives.
set(CPACK_INSTALL_CMAKE_PROJECTS
  "${CMAKE_BINARY_DIR};${PROJECT_NAME};${PYCHRON_COMPONENT};/"
  "${CMAKE_BINARY_DIR};${PROJECT_NAME};pychron_qt;/")

if(APPLE)
  set(CPACK_GENERATOR "DragNDrop")
  set(CPACK_DMG_VOLUME_NAME "Pychron")
  set(CPACK_PACKAGE_ICON "${PROJECT_SOURCE_DIR}/packaging/icons/pychron.icns")  # the mounted volume
  set(CPACK_PACKAGE_FILE_NAME "Pychron-${PROJECT_VERSION}-macOS-${CMAKE_SYSTEM_PROCESSOR}")
  # Pychron.app is signed in the staging directory before the image is made,
  # with the identity in PYCHRON_CODESIGN_IDENTITY (none set: left unsigned).
  # The release workflow signs, notarizes and staples the image itself.
  set(CPACK_PRE_BUILD_SCRIPTS "${PROJECT_SOURCE_DIR}/packaging/macos/cpack_sign.cmake")
  set(CPACK_PYCHRON_SIGN_SCRIPT "${PROJECT_SOURCE_DIR}/packaging/macos/sign_app.sh")
  set(CPACK_PYCHRON_ENTITLEMENTS "${PROJECT_SOURCE_DIR}/packaging/macos/entitlements.plist")
elseif(WIN32)
  set(CPACK_GENERATOR "NSIS;ZIP")
  set(CPACK_PACKAGE_FILE_NAME "Pychron-${PROJECT_VERSION}-windows-x64")
  set(CPACK_NSIS_DISPLAY_NAME "Pychron")
  set(CPACK_NSIS_PACKAGE_NAME "Pychron ${PROJECT_VERSION}")
  set(CPACK_NSIS_MODIFY_PATH ON)
  set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
  set(CPACK_NSIS_URL_INFO_ABOUT "${CPACK_PACKAGE_HOMEPAGE_URL}")
  set(CPACK_NSIS_MUI_ICON "${PROJECT_SOURCE_DIR}/packaging/icons/pychron.ico")
  set(CPACK_NSIS_MUI_UNIICON "${PROJECT_SOURCE_DIR}/packaging/icons/pychron.ico")
  if(TARGET pychron-ui)
    set(CPACK_PACKAGE_EXECUTABLES "pychron-ui" "Pychron")
    set(CPACK_NSIS_INSTALLED_ICON_NAME "bin\\\\pychron-ui.exe")
  endif()
else()
  set(CPACK_GENERATOR "DEB;TGZ")
  set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
  set(CPACK_DEBIAN_PACKAGE_SECTION "science")
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  # libqt6sql6-sqlite: the local data-reduction database; dlopened, so
  # dpkg-shlibdeps cannot see it.
  set(CPACK_DEBIAN_PACKAGE_DEPENDS "libqt6sql6-sqlite")
  set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "libqt6sql6-psql")
  if(PYCHRON_BUNDLE_PYTHON)
    # libpython comes with the package, not from a distribution package.
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_DIRS "${PYCHRON_BUNDLE_PYTHON}/lib")
  endif()
endif()

include(CPack)
