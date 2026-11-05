# RPU Mega emulator for System 11C

This runs a compiled RPU firmware image on a simulated ATmega2560 and models the Rev 102 board at
pin level, so a host such as PinMAME sees the bus a 6802 socket would see.

To use it from another CMake project, include `rpu_avr_engine.cmake`, link the `rpu_avr_engine` target,
and set `RPU_AVR_ENGINE_NAME` to the name you want in its reports (default `rpu`).

It includes a patched copy of simavr (GPL-3.0); `simavr-src/PATCHES` lists the changes against
upstream `14e88fa`. It is source-only here. Don't distribute a binary that links it into PinMAME
without checking the two licences first.

# Native AVR engine setup

The native engine requires CMake 3.16 or newer, a C compiler and Python 3. Build
outside the source directory:

```sh
cmake -S emulation -B /tmp/rpu-avr-native
cmake --build /tmp/rpu-avr-native --parallel 2
```

This builds the static engine, shared engine and `rpu_avr_cli`. It does not run
firmware. Neither `simavr-src/sim_core_config.h` nor `sim_core_decl.h` is required
in the source tree. CMake generates both under the build directory from the exact
core-source list that it compiles, with dependencies on that list, each core and
the generator script. Adding/removing a core or changing the script regenerates
the headers. No Git version lookup, source writes or vendor `make config` step is
needed.

The generator preserves filename-based CONFIG macros, version `unknown`, and the
existing enabled-device/alias registration policy. Its sorted declaration order
is deterministic; it does not change the existing MEGA169/MEGA169P mismatch.

Both CMake engine targets privately select their generated headers by absolute
include macros. This prevents stale source headers, including headers beside the
vendor C files, from shadowing the build outputs. The vendor sources retain their
original quoted includes when these opt-in macros are unset, preserving non-CMake
builds. The generated files are build outputs and must not be committed.

The recipe affects only native simulator setup. It does not change Arduino/RPU
firmware sources, bus behavior, clock/serve timing, or hardware clearance.
