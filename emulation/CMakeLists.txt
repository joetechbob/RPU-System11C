cmake_minimum_required(VERSION 3.16)
project(diner_rpu_avr_engine C)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
include(${CMAKE_CURRENT_LIST_DIR}/diner_rpu_avr_engine.cmake)

# Shared build of the same engine, for ctypes-driven tests.
add_library(diner_rpu_avr_engine_shared SHARED ${DINER_RPU_AVR_ENGINE_SOURCES})
add_dependencies(diner_rpu_avr_engine_shared diner_rpu_simavr_headers)
set_target_properties(diner_rpu_avr_engine_shared PROPERTIES OUTPUT_NAME diner_rpu_avr_engine C_STANDARD 99)
target_include_directories(diner_rpu_avr_engine_shared PRIVATE
  ${DINER_RPU_AVR_GENERATED_INCLUDE_DIR} ${CMAKE_CURRENT_LIST_DIR} ${DINER_RPU_AVR_SIMAVR_DIR} ${DINER_RPU_AVR_SIMAVR_DIR}/sim ${DINER_RPU_AVR_SIMAVR_DIR}/cores)
target_compile_options(diner_rpu_avr_engine_shared PRIVATE -O2 -w)
target_compile_definitions(diner_rpu_avr_engine_shared PRIVATE _GNU_SOURCE _DEFAULT_SOURCE ${DINER_RPU_AVR_GENERATED_HEADER_DEFINITIONS})

add_executable(rpu_avr_cli rpu_avr_cli.c)
target_link_libraries(rpu_avr_cli PRIVATE diner_rpu_avr_engine)
