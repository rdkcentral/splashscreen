##############################################################################
# If not stated otherwise in this file or this component's LICENSE file the
# following copyright and licenses apply:
#
# Copyright 2026 RDK Management
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
##############################################################################

cmake_minimum_required(VERSION 3.10)

project(rdke_splash_opengles LANGUAGES CXX)

include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(JPEG REQUIRED)
find_package(PNG REQUIRED)
find_package(ZLIB REQUIRED)

# Essos/Westeros are typically discovered via pkg-config in RDK/RDKE SDKs.
pkg_check_modules(ESSOS REQUIRED IMPORTED_TARGET essos)

# Yocto/sysroot note:
# Some Essos builds ship a pkg-config file that still contains an unexpanded
# autoconf placeholder '@PTHREAD_CFLAGS@'. CMake will pass it through as a raw
# token in the compiler command line, causing the build to fail.
# Filter that token out and rely on Threads::Threads instead.
if (TARGET PkgConfig::ESSOS)
  get_target_property(_essos_compile_opts PkgConfig::ESSOS INTERFACE_COMPILE_OPTIONS)
  if (_essos_compile_opts)
    set(_essos_compile_opts_sanitized "")
    foreach(_opt IN LISTS _essos_compile_opts)
      if (_opt STREQUAL "@PTHREAD_CFLAGS@")
        message(WARNING "Essos pkg-config exported unexpanded token '@PTHREAD_CFLAGS@' - filtering it out")
      else()
        list(APPEND _essos_compile_opts_sanitized "${_opt}")
      endif()
    endforeach()
    set_property(TARGET PkgConfig::ESSOS PROPERTY INTERFACE_COMPILE_OPTIONS "${_essos_compile_opts_sanitized}")
  endif()

  get_target_property(_essos_link_opts PkgConfig::ESSOS INTERFACE_LINK_OPTIONS)
  if (_essos_link_opts)
    set(_essos_link_opts_sanitized "")
    foreach(_opt IN LISTS _essos_link_opts)
      if (_opt STREQUAL "@PTHREAD_CFLAGS@")
        message(WARNING "Essos pkg-config exported unexpanded token '@PTHREAD_CFLAGS@' in link options - filtering it out")
      else()
        list(APPEND _essos_link_opts_sanitized "${_opt}")
      endif()
    endforeach()
    set_property(TARGET PkgConfig::ESSOS PROPERTY INTERFACE_LINK_OPTIONS "${_essos_link_opts_sanitized}")
  endif()
endif()

# Westeros GL may be present in the sysroot without a pkg-config file.
# Prefer a direct library lookup first (works reliably in Yocto), then fall
# back to pkg-config for environments that provide a module.
find_library(WESTEROS_GL_LIBRARY NAMES westeros_gl)

# Different platforms/layers use different module names for the same library.
# Allow override and try common defaults.
set(WESTEROS_GL_PKGCONFIG_NAME "" CACHE STRING "pkg-config module name for Westeros GL (e.g. westeros-gl or westeros_gl)")

if (NOT WESTEROS_GL_LIBRARY)
  if (WESTEROS_GL_PKGCONFIG_NAME)
    pkg_check_modules(WESTEROS_GL QUIET IMPORTED_TARGET ${WESTEROS_GL_PKGCONFIG_NAME})
  else()
    pkg_check_modules(WESTEROS_GL QUIET IMPORTED_TARGET westeros_gl)
    if (NOT WESTEROS_GL_FOUND)
      pkg_check_modules(WESTEROS_GL QUIET IMPORTED_TARGET westeros-gl)
    endif()
    if (NOT WESTEROS_GL_FOUND)
      pkg_check_modules(WESTEROS_GL QUIET IMPORTED_TARGET westerosgl)
    endif()
  endif()
endif()

if (NOT WESTEROS_GL_LIBRARY AND NOT WESTEROS_GL_FOUND)
  message(FATAL_ERROR "Westeros GL not found. Tried library libwesteros_gl and pkg-config modules: westeros_gl, westeros-gl, westerosgl (or WESTEROS_GL_PKGCONFIG_NAME override)")
endif()

# EGL/GLESv2: locate both headers and libraries (important for sysroot builds).
find_path(EGL_INCLUDE_DIR NAMES EGL/egl.h)
find_library(EGL_LIBRARY NAMES EGL)
find_path(GLES2_INCLUDE_DIR NAMES GLES2/gl2.h)
find_library(GLESv2_LIBRARY NAMES GLESv2)

if (NOT EGL_INCLUDE_DIR OR NOT EGL_LIBRARY)
  message(FATAL_ERROR "EGL not found (missing EGL/egl.h and/or libEGL)")
endif()
if (NOT GLES2_INCLUDE_DIR OR NOT GLESv2_LIBRARY)
  message(FATAL_ERROR "GLESv2 not found (missing GLES2/gl2.h and/or libGLESv2)")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(rdke_splash
  src/splashscreen.cpp
)

target_include_directories(rdke_splash PRIVATE
  src
  ${EGL_INCLUDE_DIR}
  ${GLES2_INCLUDE_DIR}
)

target_link_libraries(rdke_splash PRIVATE
  PkgConfig::ESSOS
  Threads::Threads
  ${EGL_LIBRARY}
  ${GLESv2_LIBRARY}
  JPEG::JPEG
  PNG::PNG
  ZLIB::ZLIB
)

if (WESTEROS_GL_FOUND)
  target_link_libraries(rdke_splash PRIVATE PkgConfig::WESTEROS_GL)
else()
  target_link_libraries(rdke_splash PRIVATE ${WESTEROS_GL_LIBRARY})
endif()

install(TARGETS rdke_splash
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(PROGRAMS dismiss_splash.sh
  DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES rdke_splash.service
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/system
)

