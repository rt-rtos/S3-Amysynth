# Standalone GCC LTO for selected IDF components.
#
# Replaces espressif/cmake_utilities' cu_gcc_lto_set: that macro gates on
# check_ipo_supported(), which does not reliably evaluate true against the
# ESP-IDF cross toolchain and can abort configure with FATAL_ERROR, and it
# redirects CMAKE_AR/CMAKE_RANLIB to gcc-ar/gcc-ranlib wrappers that are not
# guaranteed present. This version applies the same compile/link flags with no
# probe, no archiver override, and no Kconfig dependency.
#
# Usage (after project()):
#   include(${CMAKE_CURRENT_LIST_DIR}/cmake/gcc_lto.cmake)
#   if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND AMYSYNTH_LTO)
#       gcc_lto_enable(COMPONENTS main synth_core ...)
#   endif()
#
# A/B builds: idf.py build -DAMYSYNTH_LTO=OFF (full clean between flips —
# incremental builds lie under LTO).
#
# GCC only: clang rejects -ffat-lto-objects / -flto-compression-level, so the
# caller must gate on CMAKE_C_COMPILER_ID.

option(AMYSYNTH_LTO "Compile the hot components with GCC LTO" ON)

# ltrans partitioning: "max" favors parallel link;
# "one"/"none" compile the whole program as a single ltrans unit, widening
# cross-TU inlining scope at the cost of link time. A/B via
# -DAMYSYNTH_LTO_PARTITION=one + full clean; ELF codegen diff is the judge.
set(AMYSYNTH_LTO_PARTITION "one" CACHE STRING "GCC -flto-partition mode (max|one|none|balanced)")

# -ffat-lto-objects keeps full native code alongside the LTO IR so plain
# ar/nm/objdump (and asmdiff object mode) keep working on the archives.
# -Werror=lto-type-mismatch: LTO sees all TUs at once, so cross-TU
# declaration/definition mismatches become detectable - fail the link on them.
# Unexplored candidate: the whole-program inline budget
# (--param inline-unit-growth=, --param max-inline-insns-auto=) binds harder
# under LTO than per-TU; a bump may unlock hot-path inlining, but verify IRAM
# growth and ZOL survival (register pressure) before adopting.
# LTO IR compression inside the fat objects: build-time/disk trade only, the
# IR is decompressed before ltrans so codegen should be identical at any level.
set(AMYSYNTH_LTO_COMPRESSION "9" CACHE STRING "GCC -flto-compression-level (0-19)")

set(GCC_LTO_COMPILE_OPTIONS -flto=auto -ffat-lto-objects
                            -flto-compression-level=${AMYSYNTH_LTO_COMPRESSION})
# -mtext-section-literals: each function's literals go in its own text
# section, beside it. A single ltrans unit (partition "one") otherwise keeps
# one literal pool for the whole program, and a function placed more than
# 256 KB after it is out of l32r's reach ("dangerous relocation: l32r:
# literal target out of range"); app_main hit that as the unit grew.
set(GCC_LTO_LINK_OPTIONS    -flto -fuse-linker-plugin -ffat-lto-objects
                            -flto-partition=${AMYSYNTH_LTO_PARTITION}
                            -mtext-section-literals
                            -Werror=lto-type-mismatch)

function(gcc_lto_enable)
    cmake_parse_arguments(LTO "" "" "COMPONENTS" ${ARGN})
    message(STATUS "GCC LTO for components: ${LTO_COMPONENTS}")
    foreach(c ${LTO_COMPONENTS})
        idf_component_get_property(t ${c} COMPONENT_LIB)
        target_compile_options(${t} PRIVATE ${GCC_LTO_COMPILE_OPTIONS})
    endforeach()
    target_link_libraries(${project_elf} PRIVATE ${GCC_LTO_LINK_OPTIONS})
endfunction()
