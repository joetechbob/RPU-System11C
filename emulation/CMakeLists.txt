cmake_minimum_required(VERSION 3.16)
project(rpu_avr_engine C)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
include(${CMAKE_CURRENT_LIST_DIR}/rpu_avr_engine.cmake)

# Shared build of the same engine, for ctypes-driven tests.
add_library(rpu_avr_engine_shared SHARED ${RPU_AVR_ENGINE_SOURCES})
add_dependencies(rpu_avr_engine_shared rpu_avr_simavr_headers)
set_target_properties(rpu_avr_engine_shared PROPERTIES OUTPUT_NAME rpu_avr_engine C_STANDARD 99)
target_include_directories(rpu_avr_engine_shared PRIVATE
  ${RPU_AVR_GENERATED_INCLUDE_DIR} ${CMAKE_CURRENT_LIST_DIR} ${RPU_AVR_SIMAVR_DIR} ${RPU_AVR_SIMAVR_DIR}/sim ${RPU_AVR_SIMAVR_DIR}/cores)
target_compile_options(rpu_avr_engine_shared PRIVATE -O2 -w)
target_compile_definitions(rpu_avr_engine_shared PRIVATE _GNU_SOURCE _DEFAULT_SOURCE ${RPU_AVR_GENERATED_HEADER_DEFINITIONS})

add_executable(rpu_avr_cli rpu_avr_cli.c)
target_link_libraries(rpu_avr_cli PRIVATE rpu_avr_engine)
