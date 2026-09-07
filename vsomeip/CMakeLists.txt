# CMakeLists.txt
cmake_minimum_required(VERSION 3.10)
project(vsomeip_adas CXX)

# ---- Language / standard ----
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Optional, off by default (no behavioral change)
option(ENABLE_LTO "Enable interprocedural optimization (link-time optimization)" OFF)

# ---- Warnings ----
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---- vSomeIP ----
find_path(VSOMEIP_INCLUDE_DIR vsomeip/vsomeip.hpp)
find_library(VSOMEIP_LIB NAMES vsomeip3 vsomeip)

if (NOT VSOMEIP_INCLUDE_DIR OR NOT VSOMEIP_LIB)
  message(FATAL_ERROR "vSomeIP not found. Install libvsomeip3-dev or build vsomeip from source.")
else()
  message(STATUS "Found vSomeIP include: ${VSOMEIP_INCLUDE_DIR}")
  message(STATUS "Found vSomeIP lib:     ${VSOMEIP_LIB}")
endif()

# ---- Boost ----
find_package(Boost REQUIRED COMPONENTS system thread log)
message(STATUS "Boost include: ${Boost_INCLUDE_DIRS}")
message(STATUS "Boost libs:    ${Boost_LIBRARIES}")

# ---- Threads (pthread) ----
find_package(Threads REQUIRED)

# ---- Zlib (xlsxwriter dependency) ----
find_package(ZLIB REQUIRED)

# ---- Excel writer (libxlsxwriter) ----
find_library(XLSXWRITER_LIB xlsxwriter)
if (NOT XLSXWRITER_LIB)
  message(FATAL_ERROR "libxlsxwriter not found. Install libxlsxwriter-dev.")
else()
  message(STATUS "Found xlsxwriter lib: ${XLSXWRITER_LIB}")
endif()

# ===================== Targets =====================

# ---- radar publisher (single binary, multi-radar via --slots/--radar-ids) ----
add_executable(radar_publisher radar_publisher.cpp)
target_include_directories(radar_publisher PRIVATE
  ${VSOMEIP_INCLUDE_DIR}
  ${Boost_INCLUDE_DIRS}
)
target_link_libraries(radar_publisher PRIVATE
  ${VSOMEIP_LIB}
  ${Boost_LIBRARIES}
  Threads::Threads
)

# ---- ADAS subscriber (ECU) ----
add_executable(adas_subscriber adas_subscriber.cpp)
target_include_directories(adas_subscriber PRIVATE
  ${VSOMEIP_INCLUDE_DIR}
  ${Boost_INCLUDE_DIRS}
)
target_compile_definitions(adas_subscriber PRIVATE USE_XLSX)
target_link_libraries(adas_subscriber PRIVATE
  ${VSOMEIP_LIB}
  ${Boost_LIBRARIES}
  Threads::Threads
  ZLIB::ZLIB
  ${XLSXWRITER_LIB}
)

# ---- GCC < 9 needs stdc++fs for <filesystem> ----
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(radar_publisher PRIVATE stdc++fs)
    target_link_libraries(adas_subscriber PRIVATE stdc++fs)
  endif()
endif()

# ---- Optional LTO (does not change behavior; OFF by default) ----
if (ENABLE_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT ipo_ok OUTPUT ipo_msg)
  if (ipo_ok)
    set_property(TARGET radar_publisher PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    set_property(TARGET adas_subscriber  PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    message(STATUS "LTO enabled for targets.")
  else()
    message(WARNING "LTO not supported: ${ipo_msg}")
  endif()
endif()

# ---- Install ----
install(TARGETS radar_publisher adas_subscriber DESTINATION bin)