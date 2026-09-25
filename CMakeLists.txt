cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.c
    src/delay-buffer.c
    src/delay-service.c
    src/remote.c
    src/settings-dialog.cpp
    src/rtmp/rtmp-stream.c
    src/rtmp/rtmp-windows.c
    src/rtmp/flv-mux.c
    src/rtmp/net-if.c
    src/rtmp/rtmp-av1.c
    src/rtmp/rtmp-hevc.c
    src/rtmp/librtmp/amf.c
    src/rtmp/librtmp/cencode.c
    src/rtmp/librtmp/hashswf.c
    src/rtmp/librtmp/log.c
    src/rtmp/librtmp/md5.c
    src/rtmp/librtmp/parseurl.c
    src/rtmp/librtmp/rtmp.c
    src/shared/happy-eyeballs.c
)

# Le code repris d'OBS (librtmp) produit des avertissements : ne pas les traiter en erreurs pour le spike.
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES COMPILE_WARNING_AS_ERROR OFF)

# RTMP sender derived from OBS's obs-outputs (GPL-2.0-or-later). Spike: plain RTMP, no RTMPS (no mbedTLS).
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE src src/rtmp src/shared)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE NO_CRYPTO ENABLE_HEVC)

if(OS_WINDOWS)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::w32-pthreads iphlpapi winmm ws2_32 winhttp)
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
