cmake_minimum_required(VERSION 3.0)

set(CMAKE_CXX_STANDARD 14)

project(ReStreamer)

if(DEFINED ENV{SNAPCRAFT_PROJECT_NAME})
    add_definitions(-DSNAPCRAFT_BUILD=1)
    set(SNAPCRAFT_BUILD YES)
endif()

add_subdirectory(WebRTSP)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_search_module(LIBCONFIG REQUIRED libconfig)

file(GLOB SOURCES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    *.cpp
    *.h
    *.conf.sample
    README.md
    BUILD.md
    *.sh
    *.cmake)

file(GLOB_RECURSE SNAP RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    snap/*)

file(GLOB_RECURSE WWW RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    www/[^.]*)

add_executable(${PROJECT_NAME} ${SOURCES} ${SNAP} ${WWW})
target_include_directories(${PROJECT_NAME} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/WebRTSP
    ${LIBCONFIG_INCLUDE_DIRS}
    ${SPDLOG_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME}
    ${LIBCONFIG_LIBRARIES}
    ${SPDLOG_LDFLAGS}
    Http
    RtspParser
    Client
    Signalling
    RtStreaming)

if(SNAPCRAFT_BUILD)
    install(TARGETS ${PROJECT_NAME} DESTINATION bin)
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/restreamer.conf.sample DESTINATION etc)
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/www
        DESTINATION /opt/$ENV{SNAPCRAFT_PROJECT_NAME}
        FILES_MATCHING
        PATTERN "www/3rd_party/*.js"
        PATTERN "www/3rd_party/*.css"
        PATTERN "www/WebRTSP/*.mjs"
        PATTERN "www/css/*.css"
        PATTERN "www/index.html"
        PATTERN "www/favicon.ico")
endif()
