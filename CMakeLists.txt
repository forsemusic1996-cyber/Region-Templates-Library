cmake_minimum_required(VERSION 3.20)
project(RegionTemplatesFastSync LANGUAGES C CXX)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(RT_CURL_SOURCE "" CACHE PATH "Path to curl source for a self-contained build")
if(RT_CURL_SOURCE)
  set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(CURL_TARGET_WINDOWS_VERSION "0x0601" CACHE STRING "" FORCE)
  set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
  set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
  set(CURL_ZLIB OFF CACHE STRING "" FORCE)
  set(CURL_BROTLI OFF CACHE STRING "" FORCE)
  set(CURL_ZSTD OFF CACHE STRING "" FORCE)
  set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
  set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
  set(CURL_USE_LIBSSH OFF CACHE BOOL "" FORCE)
  set(CURL_USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
  set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
  set(CURL_DISABLE_LDAPS ON CACHE BOOL "" FORCE)
  set(HTTP_ONLY ON CACHE BOOL "" FORCE)
  add_subdirectory("${RT_CURL_SOURCE}" curl-build)
else()
  find_package(CURL REQUIRED)
endif()

add_library(RegionTemplatesNet SHARED
  src/region_templates_net.cpp
)

target_include_directories(RegionTemplatesNet PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(RegionTemplatesNet PRIVATE CURL::libcurl)

if(WIN32)
  target_link_libraries(RegionTemplatesNet PRIVATE ws2_32 crypt32 wldap32 normaliz)
  target_compile_definitions(RegionTemplatesNet PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601 NOMINMAX)
  if(MINGW)
    # REAPER must be able to load the plugin without separate MinGW runtime
    # DLLs next to it.
    target_link_options(RegionTemplatesNet PRIVATE
      -static
      -static-libgcc
      -static-libstdc++
    )
  endif()
  set_target_properties(RegionTemplatesNet PROPERTIES PREFIX "" SUFFIX ".dll")
elseif(APPLE)
  set_target_properties(RegionTemplatesNet PROPERTIES PREFIX "" SUFFIX ".dylib")
else()
  set_target_properties(RegionTemplatesNet PROPERTIES PREFIX "" SUFFIX ".so")
endif()

set_target_properties(RegionTemplatesNet PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES
)
