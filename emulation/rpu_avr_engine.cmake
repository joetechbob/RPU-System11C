# Include from PinMAME's (or any) CMake build:
#
#   set(RPU_AVR_SIMAVR_DIR /path/to/simavr-copy)   # dir holding sim/ and cores/
#   include(/path/to/avrsim/rpu_avr_engine.cmake)
#   target_link_libraries(pinmame PRIVATE rpu_avr_engine)
#
# Defaults to the simavr copy checked out next to this file (simavr-src).
# Produces the static library target `rpu_avr_engine` (engine + the
# simavr sources it needs, no libelf) and exports RPU_AVR_ENGINE_INCLUDE_DIR.
if(NOT DEFINED RPU_AVR_SIMAVR_DIR)
  set(RPU_AVR_SIMAVR_DIR "${CMAKE_CURRENT_LIST_DIR}/simavr-src")
endif()
# The machine name used in the engine's reports; a build can set its own.
set(RPU_AVR_ENGINE_NAME "rpu" CACHE STRING "Machine name in engine reports")
set(RPU_AVR_ENGINE_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(RPU_AVR_ENGINE_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}")

if(NOT TARGET rpu_avr_engine)
  file(GLOB _dr_sim_sources "${RPU_AVR_SIMAVR_DIR}/sim/sim_*.c" "${RPU_AVR_SIMAVR_DIR}/sim/avr_*.c")
  file(GLOB _dr_core_sources CONFIGURE_DEPENDS "${RPU_AVR_SIMAVR_DIR}/cores/sim_*.c")
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  # CMake owns these outputs; an absent or stale source-tree header is irrelevant.
  set(RPU_AVR_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/rpu_avr_simavr_generated")
  file(MAKE_DIRECTORY "${RPU_AVR_GENERATED_INCLUDE_DIR}")
  set(_dr_core_input_list "${RPU_AVR_GENERATED_INCLUDE_DIR}/core-inputs.txt")
  file(GENERATE OUTPUT "${_dr_core_input_list}" CONTENT "${_dr_core_sources}\n")
  add_custom_command(
    OUTPUT "${RPU_AVR_GENERATED_INCLUDE_DIR}/sim_core_config.h"
           "${RPU_AVR_GENERATED_INCLUDE_DIR}/sim_core_decl.h"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/generate_simavr_headers.py"
            --output-dir "${RPU_AVR_GENERATED_INCLUDE_DIR}" ${_dr_core_sources}
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/generate_simavr_headers.py"
            "${CMAKE_CURRENT_LIST_FILE}" "${_dr_core_input_list}" ${_dr_core_sources}
    VERBATIM)
  add_custom_target(rpu_avr_simavr_headers DEPENDS
    "${RPU_AVR_GENERATED_INCLUDE_DIR}/sim_core_config.h"
    "${RPU_AVR_GENERATED_INCLUDE_DIR}/sim_core_decl.h")
  # Absolute opt-in include macros also defeat quoted-header lookup in the
  # vendor source directory. Unmodified non-CMake builds keep their old path.
  set(RPU_AVR_GENERATED_HEADER_DEFINITIONS
    "RPU_AVR_SIMAVR_CORE_CONFIG_HEADER=\"${RPU_AVR_GENERATED_INCLUDE_DIR}/sim_core_config.h\""
    "RPU_AVR_SIMAVR_CORE_DECL_HEADER=\"${RPU_AVR_GENERATED_INCLUDE_DIR}/sim_core_decl.h\""
    "RPU_AVR_ENGINE_NAME=\"${RPU_AVR_ENGINE_NAME}\"")
  # ELF/DWARF/hex loaders need libelf and gdb needs them; the engine takes raw flash bytes
  # and avr_gdb_stubs.c replaces the gdb server.
  list(FILTER _dr_sim_sources EXCLUDE REGEX "/sim_(elf|dwarf|hex|gdb)\\.c$")
  set(RPU_AVR_ENGINE_SOURCES ${CMAKE_CURRENT_LIST_DIR}/rpu_avr_engine.c ${CMAKE_CURRENT_LIST_DIR}/avr_gdb_stubs.c ${_dr_sim_sources} ${_dr_core_sources})
  add_library(rpu_avr_engine STATIC ${RPU_AVR_ENGINE_SOURCES})
  add_dependencies(rpu_avr_engine rpu_avr_simavr_headers)
  set_target_properties(rpu_avr_engine PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 99)
  target_include_directories(rpu_avr_engine
    PUBLIC ${CMAKE_CURRENT_LIST_DIR}
    PRIVATE ${RPU_AVR_GENERATED_INCLUDE_DIR} ${RPU_AVR_SIMAVR_DIR} ${RPU_AVR_SIMAVR_DIR}/sim ${RPU_AVR_SIMAVR_DIR}/cores)
  target_compile_options(rpu_avr_engine PRIVATE -O2 -w)
  target_compile_definitions(rpu_avr_engine PRIVATE _GNU_SOURCE _DEFAULT_SOURCE ${RPU_AVR_GENERATED_HEADER_DEFINITIONS})
  find_package(Threads)
  if(Threads_FOUND)
    target_link_libraries(rpu_avr_engine PUBLIC Threads::Threads)
  endif()
endif()
