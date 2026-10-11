# Routes compiles through ccache when it is installed, so a rebuild, a fresh
# build directory or a new worktree reuses objects already compiled anywhere
# on this machine.
#
# ccache's base_dir is set to the source tree, so paths are hashed relative to
# it: a worktree under .claude/worktrees/<name> hits the objects its main
# checkout (or any other worktree) already built. hash_dir is off so the build
# directory's absolute path does not enter the key either; debug info may then
# name the directory that first compiled an object.
#
# A launcher already given (CI sets CMAKE_CXX_COMPILER_LAUNCHER) is left
# alone. -DPYCHRON_CCACHE=OFF turns this off. Visual Studio generators do not
# support launchers and are skipped.

option(PYCHRON_CCACHE "Compile through ccache when it is found" ON)

if(NOT PYCHRON_CCACHE OR CMAKE_CXX_COMPILER_LAUNCHER)
  return()
endif()
if(NOT CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
  return()
endif()

find_program(PYCHRON_CCACHE_PROGRAM ccache)
if(NOT PYCHRON_CCACHE_PROGRAM)
  message(STATUS "ccache not found; compiling without it (brew install ccache)")
  return()
endif()

set(_pychron_ccache_launcher
  "${CMAKE_COMMAND}" -E env
  "CCACHE_BASEDIR=${PROJECT_SOURCE_DIR}"
  CCACHE_NOHASHDIR=1
  "${PYCHRON_CCACHE_PROGRAM}")
set(CMAKE_C_COMPILER_LAUNCHER ${_pychron_ccache_launcher})
set(CMAKE_CXX_COMPILER_LAUNCHER ${_pychron_ccache_launcher})
message(STATUS "Compiling through ccache: ${PYCHRON_CCACHE_PROGRAM}")
