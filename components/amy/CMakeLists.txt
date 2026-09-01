file(GLOB AMY_SRC_FILES LIST_DIRECTORIES OFF "${CMAKE_CURRENT_LIST_DIR}/src/*.c")

# Exclude standalone examples and non-ESP drivers
list(FILTER AMY_SRC_FILES EXCLUDE REGEX "amy-example\\.c$")
list(FILTER AMY_SRC_FILES EXCLUDE REGEX "libminiaudio-audio\\.c$")
list(FILTER AMY_SRC_FILES EXCLUDE REGEX "usb\\.c$")

if(NOT AMY_SRC_FILES)
    message(FATAL_ERROR "No AMY sources found in ${CMAKE_CURRENT_LIST_DIR}/src. Make sure the `src` directory exists within the component.")
endif()

idf_component_register(
    SRCS ${AMY_SRC_FILES}
    INCLUDE_DIRS "src"
    REQUIRES esp_timer esp_driver_uart esp_driver_i2s esp_driver_i2c esp_driver_gpio espressif__tinyusb
)
option(AMYSYNTH_AMY_O3 "Compile the amy component at -O3" OFF)
if(AMYSYNTH_AMY_O3)
    target_compile_options(${COMPONENT_LIB} PRIVATE -O3)
endif()
# Profiling mode (Kconfig: AMY_PROFILE_MODE). FULL maps to AMY's upstream
# AMY_DEBUG (all tags). COARSE is a local addition that compiles only the outer
# render-stage tags (see AMY-EDITS.md). OFF defines neither and the macros are
# no-ops. PUBLIC so a consumer that includes amy.h sees consistent macros.
if(CONFIG_AMY_PROFILE_FULL)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC AMY_DEBUG)
elseif(CONFIG_AMY_PROFILE_COARSE)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC AMY_PROFILE_COARSE)
endif()

# Wavetable oscillator (Kconfig: AMY_WAVETABLE). PUBLIC so consumers that
# include amy.h (synth_core) see the same wave=WAVETABLE support and can read
# pcm_wavetable_base/samples/len to route virtual wavetable patches.
if(CONFIG_AMY_WAVETABLE)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC AMY_WAVETABLE)
endif()

# Gamma TR-808 PCM bank (Kconfig: AMY_PCM_GAMMA808). Maps to upstream's
# GAMMA9001 switch: amy.c includes pcm_gamma808.h instead of pcm_tiny.h,
# patches.h picks the matching patch-258 drum map, and pcm.c/amy.h compile
# the (inert until fed a blob) gamma9001 streaming hooks. PUBLIC so consumers
# that include amy.h see the same amy_set_gamma9001_pcm() declaration.
if(CONFIG_AMY_PCM_GAMMA808)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC GAMMA9001)

    # Gamma9001 sample blob for the 'drums' data partition (regenerate with
    # tools/gen-gamma9001-drums.py, or upstream
    # amy.headers.generate_gamma9001_headers()). Missing blob is not an
    # error - boot then leaves presets 256+ disabled.
    set(AMY_DRUMS_BIN "${CMAKE_CURRENT_LIST_DIR}/drums.bin")
    if(EXISTS ${AMY_DRUMS_BIN})
        # Deliberately NOT part of the default flash target: the blob only
        # changes when regenerated, and rewriting 3.7 MB costs ~40 s on every
        # flash. Write it once (and after regenerating) with `idf.py drums-flash`.
        idf_component_get_property(esptool_main_args esptool_py FLASH_ARGS)
        idf_component_get_property(esptool_sub_args esptool_py FLASH_SUB_ARGS)
        esptool_py_flash_target(drums-flash "${esptool_main_args}" "${esptool_sub_args}")
        esptool_py_flash_to_partition(drums-flash "drums" ${AMY_DRUMS_BIN})
    else()
        message(STATUS "amy: drums.bin not present; gamma9001 presets 256+ will be unavailable")
    endif()
endif()

# Keep the same compile flag for amy.c (relax strict-aliasing warnings)
set(AMY_CORE_C "${CMAKE_CURRENT_LIST_DIR}/src/amy.c")
if(EXISTS ${AMY_CORE_C})
    set_source_files_properties(${AMY_CORE_C} PROPERTIES COMPILE_FLAGS "-Wno-strict-aliasing")
endif()

# PERF (2026-06): drop assert() from the pure-DSP hot-path files only. The build
# enables assertions globally (CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE),
# which emits runtime assert checks inside the per-sample render/filter loops
# (e.g. filters.c dsps_biquad_*_split_fb assert(FILTER_SCALEUP_BITS==0)). These
# files contain no structural / cross-core state, so NDEBUG here removes the
# checks without weakening the safety asserts in amy.c (osc alloc/free, delta
# queue, render lock) or anywhere else in the project.
set(AMY_DSP_HOT_FILES
    "${CMAKE_CURRENT_LIST_DIR}/src/filters.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/oscillators.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/envelope.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/delay.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/log2_exp2.c"
)
foreach(_dsp_file ${AMY_DSP_HOT_FILES})
    if(EXISTS ${_dsp_file})
        set_property(SOURCE ${_dsp_file} APPEND_STRING PROPERTY COMPILE_FLAGS " -DNDEBUG")
    endif()
endforeach()
