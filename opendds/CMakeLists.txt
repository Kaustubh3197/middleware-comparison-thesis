cmake_minimum_required(VERSION 3.16)
project(opendds_adas LANGUAGES CXX)

# ---- Language / tooling ----
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # for IDEs / VS Code

# ---- OpenDDS ----
# Provides OpenDDS::Dcps + opendds_target_sources()
find_package(OpenDDS REQUIRED)

# ---- Optional Excel export (.xlsx) for ECU logs ----
option(USE_XLSXWRITER "Enable writing .xlsx logs (ECU side)" ON)

if(USE_XLSXWRITER)
  # Try to find headers and library
  find_path(XLSXWRITER_INCLUDE_DIR NAMES xlsxwriter.h)
  find_library(XLSXWRITER_LIB NAMES xlsxwriter)
  find_package(ZLIB REQUIRED)

  if(NOT XLSXWRITER_INCLUDE_DIR)
    message(FATAL_ERROR "xlsxwriter.h not found. Install 'libxlsxwriter-dev' or set XLSXWRITER_INCLUDE_DIR.")
  endif()
  if(NOT XLSXWRITER_LIB)
    message(FATAL_ERROR "libxlsxwriter not found. Install 'libxlsxwriter-dev' or set XLSXWRITER_LIB.")
  endif()

  message(STATUS "Excel export: ON")
  message(STATUS "  libxlsxwriter include: ${XLSXWRITER_INCLUDE_DIR}")
  message(STATUS "  libxlsxwriter library: ${XLSXWRITER_LIB}")
else()
  message(STATUS "Excel export: OFF (USE_XLSXWRITER=OFF)")
endif()

# ===================== Radar publisher =====================
add_executable(radar_publisher
  src/radar_publisher.cpp
)
opendds_target_sources(radar_publisher PUBLIC idl/AdasZenohCompat.idl)
target_link_libraries(radar_publisher PUBLIC OpenDDS::Dcps)
target_include_directories(radar_publisher PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}/opendds_generated
)

# ===================== ECU subscriber ======================
add_executable(ecu_subscriber
  src/ecu_subscriber.cpp
)
opendds_target_sources(ecu_subscriber PUBLIC idl/AdasZenohCompat.idl)
target_link_libraries(ecu_subscriber PUBLIC OpenDDS::Dcps)
target_include_directories(ecu_subscriber PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}/opendds_generated
)

# Link Excel lib only to ECU and set the define only for that target
if(USE_XLSXWRITER)
  target_link_libraries(ecu_subscriber PRIVATE ${XLSXWRITER_LIB} ZLIB::ZLIB)
  target_include_directories(ecu_subscriber PRIVATE ${XLSXWRITER_INCLUDE_DIR})
  target_compile_definitions(ecu_subscriber PRIVATE USE_XLSXWRITER)
endif()