cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)
# ON only for testing: talks to the Dev database and the local site, and shows the testing options.
option(SV_DEV_BUILD "Dev build: Dev database, local site, testing options" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core Svg)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets Qt6::Svg)
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
    src/plugin-main.cpp
    src/sv-source.cpp
    src/sv-layout.cpp
    src/sv-mask.cpp
    src/sv-link.cpp
    src/sv-store.cpp
    src/sv-detect.cpp
    src/sv-canvas.cpp
    src/sv-edit.cpp
    src/sv-output.cpp
    src/sv-actions.cpp
    src/sv-ui.cpp
    src/sv-look.cpp
    src/sv-zoom.cpp
    src/sv-remote.cpp
    src/sv-follow.cpp
    src/sv-sound.cpp
    src/sv-brand.cpp
    src/sv-replay.cpp
)

if(SV_DEV_BUILD)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE SV_DEV_BUILD)
endif()

if(OS_WINDOWS)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE winhttp crypt32 shell32 ws2_32)
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
