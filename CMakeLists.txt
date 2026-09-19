cmake_minimum_required(VERSION 3.20)
project(RegionTemplatesFastSync LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(CURL REQUIRED)

add_library(RegionTemplatesNet SHARED
  src/region_templates_net.cpp
)

target_include_directories(RegionTemplatesNet PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(RegionTemplatesNet PRIVATE CURL::libcurl)

if(WIN32)
  target_link_libraries(RegionTemplatesNet PRIVATE ws2_32 crypt32 wldap32 normaliz)
  target_compile_definitions(RegionTemplatesNet PRIVATE _WIN32_WINNT=0x0601)
  if(MINGW)
    target_link_options(RegionTemplatesNet PRIVATE
      -static-libgcc
      -static-libstdc++
      -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic
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
