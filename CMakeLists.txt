cmake_minimum_required(VERSION 3.13)
project(p25_trunking CXX)

# SDR++ has no stable module ABI: build against the core source of the exact commit the
# installed SDR++ was built from (see SDRPP_COMMIT and scripts/fetch_sdrpp.sh).
set(SDRPP_CORE_ROOT "${CMAKE_SOURCE_DIR}/third_party/SDRPlusPlus/core" CACHE PATH "SDR++ core source directory")
set(SDRPP_LIB_ROOT "/usr/lib" CACHE PATH "Directory containing libsdrpp_core.so")

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

file(GLOB SRC "${CMAKE_SOURCE_DIR}/src/*.cpp")
add_library(${PROJECT_NAME} SHARED ${SRC})
target_include_directories(${PROJECT_NAME} PRIVATE "${SDRPP_CORE_ROOT}/src/" "${SDRPP_CORE_ROOT}/src/imgui/")
target_link_libraries(${PROJECT_NAME} PRIVATE "${SDRPP_LIB_ROOT}/libsdrpp_core.so" pthread)
target_compile_options(${PROJECT_NAME} PRIVATE -std=c++17 -Wall -Wno-unused-parameter $<$<CONFIG:Release>:-O3>)
set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "")

install(TARGETS ${PROJECT_NAME} DESTINATION lib/sdrpp/plugins)
