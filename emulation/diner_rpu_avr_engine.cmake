# Include from PinMAME's (or any) CMake build:
#
#   set(DINER_RPU_AVR_SIMAVR_DIR /path/to/simavr-copy)   # dir holding sim/ and cores/
#   include(/path/to/avrsim/diner_rpu_avr_engine.cmake)
#   target_link_libraries(pinmame PRIVATE diner_rpu_avr_engine)
#
# Defaults to the simavr copy checked out next to this file (simavr-src).
# Produces the static library target `diner_rpu_avr_engine` (engine + the
# simavr sources it needs, no libelf) and exports DINER_RPU_AVR_ENGINE_INCLUDE_DIR.
if(NOT DEFINED DINER_RPU_AVR_SIMAVR_DIR)
  set(DINER_RPU_AVR_SIMAVR_DIR "${CMAKE_CURRENT_LIST_DIR}/simavr-src")
endif()
set(DINER_RPU_AVR_ENGINE_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(DINER_RPU_AVR_ENGINE_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}")

if(NOT TARGET diner_rpu_avr_engine)
  file(GLOB _dr_sim_sources "${DINER_RPU_AVR_SIMAVR_DIR}/sim/sim_*.c" "${DINER_RPU_AVR_SIMAVR_DIR}/sim/avr_*.c")
  file(GLOB _dr_core_sources CONFIGURE_DEPENDS "${DINER_RPU_AVR_SIMAVR_DIR}/cores/sim_*.c")
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  # CMake owns these outputs; an absent or stale source-tree header is irrelevant.
  set(DINER_RPU_AVR_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/diner_rpu_simavr_generated")
  file(MAKE_DIRECTORY "${DINER_RPU_AVR_GENERATED_INCLUDE_DIR}")
  set(_dr_core_input_list "${DINER_RPU_AVR_GENERATED_INCLUDE_DIR}/core-inputs.txt")
  file(GENERATE OUTPUT "${_dr_core_input_list}" CONTENT "${_dr_core_sources}\n")
  add_custom_command(
    OUTPUT "${DINER_RPU_AVR_GENERATED_INCLUDE_DIR}/sim_core_config.h"
           "${DINER_RPU_AVR_GENERATED_INCLUDE_DIR}/sim_core_decl.h"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/generate_simavr_headers.py"
            --output-dir "${DINER_RPU_AVR_GENERATED_INCLUDE_DIR}" ${_dr_core_sources}
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/generate_simavr_headers.py"
            "${CMAKE_CURRENT_LIST_FILE}" "${_dr_core_input_list}" ${_dr_core_sources}
    VERBATIM)
  add_custom_target(diner_rpu_simavr_headers DEPENDS
    "${DINER_RPU_AVR_GENERATED_INCLUDE_DIR}/sim_core_config.h"
    "${DINER_RPU_AVR_GENERATED_INCLUDE_DIR}/sim_core_decl.h")
  # Absolute opt-in include macros also defeat quoted-header lookup in the
  # vendor source directory. Unmodified non-CMake builds keep their old path.
  set(DINER_RPU_AVR_GENERATED_HEADER_DEFINITIONS
    "DINER_RPU_SIMAVR_CORE_CONFIG_HEADER=\"${DINER_RPU_AVR_GENERATED_INCLUDE_DIR}/sim_core_config.h\""
    "DINER_RPU_SIMAVR_CORE_DECL_HEADER=\"${DINER_RPU_AVR_GENERATED_INCLUDE_DIR}/sim_core_decl.h\"")
  # ELF/DWARF/hex loaders need libelf and gdb needs them; the engine takes raw flash bytes
  # and avr_gdb_stubs.c replaces the gdb server.
  list(FILTER _dr_sim_sources EXCLUDE REGEX "/sim_(elf|dwarf|hex|gdb)\\.c$")
  set(DINER_RPU_AVR_ENGINE_SOURCES ${CMAKE_CURRENT_LIST_DIR}/rpu_avr_engine.c ${CMAKE_CURRENT_LIST_DIR}/avr_gdb_stubs.c ${_dr_sim_sources} ${_dr_core_sources})
  add_library(diner_rpu_avr_engine STATIC ${DINER_RPU_AVR_ENGINE_SOURCES})
  add_dependencies(diner_rpu_avr_engine diner_rpu_simavr_headers)
  set_target_properties(diner_rpu_avr_engine PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 99)
  target_include_directories(diner_rpu_avr_engine
    PUBLIC ${CMAKE_CURRENT_LIST_DIR}
    PRIVATE ${DINER_RPU_AVR_GENERATED_INCLUDE_DIR} ${DINER_RPU_AVR_SIMAVR_DIR} ${DINER_RPU_AVR_SIMAVR_DIR}/sim ${DINER_RPU_AVR_SIMAVR_DIR}/cores)
  target_compile_options(diner_rpu_avr_engine PRIVATE -O2 -w)
  target_compile_definitions(diner_rpu_avr_engine PRIVATE _GNU_SOURCE _DEFAULT_SOURCE ${DINER_RPU_AVR_GENERATED_HEADER_DEFINITIONS})
  find_package(Threads)
  if(Threads_FOUND)
    target_link_libraries(diner_rpu_avr_engine PUBLIC Threads::Threads)
  endif()
endif()
