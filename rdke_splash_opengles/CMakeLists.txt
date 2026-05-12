cmake_minimum_required(VERSION 3.10)

# Copyright 2026 RDK Management

project(rdke_splash_opengles LANGUAGES CXX)

include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)
find_package(JPEG REQUIRED)
find_package(PNG REQUIRED)
find_package(ZLIB REQUIRED)

# Essos/Westeros are typically discovered via pkg-config in RDK/RDKE SDKs.
pkg_check_modules(ESSOS REQUIRED IMPORTED_TARGET essos)

# Different platforms/layers use different module names for the same library.
# Allow override and try common defaults.
set(WESTEROS_GL_PKGCONFIG_NAME "" CACHE STRING "pkg-config module name for Westeros GL (e.g. westeros-gl or westeros_gl)")

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

if (NOT WESTEROS_GL_FOUND)
  # Some SDKs ship the library without a pkg-config file.
  find_library(WESTEROS_GL_LIBRARY NAMES westeros_gl)
  if (NOT WESTEROS_GL_LIBRARY)
    message(FATAL_ERROR "Westeros GL not found. Tried pkg-config modules: westeros_gl, westeros-gl, westerosgl (or WESTEROS_GL_PKGCONFIG_NAME override) and library libwesteros_gl")
  endif()
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
  src/main.cpp
)

target_include_directories(rdke_splash PRIVATE
  src
  ${EGL_INCLUDE_DIR}
  ${GLES2_INCLUDE_DIR}
)

target_link_libraries(rdke_splash PRIVATE
  PkgConfig::ESSOS
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

