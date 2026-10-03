# Custom DX7-style FM/ALGO voices are a WIP feature, gated off by default
# (CONFIG_SYNTH_CUSTOM_FM). When disabled, none of the FM voice/preset/editor
# code is compiled; all reference sites are guarded by #if CONFIG_SYNTH_CUSTOM_FM.
set(fm_srcs)
if(CONFIG_SYNTH_CUSTOM_FM)
    list(APPEND fm_srcs
        "custompatches/fm_voice.c"
        "custompatches/fm_graph.c"
        "custompatches/fm_presets.c"
        "synth_ui/ui_screen_fm.c")
endif()

# Additive/partials voices are likewise WIP, gated off by default
# (CONFIG_SYNTH_ADDITIVE); all reference sites are guarded by
# #if CONFIG_SYNTH_ADDITIVE. No editor screen yet (fixed presets ship first).
set(additive_srcs)
if(CONFIG_SYNTH_ADDITIVE)
    list(APPEND additive_srcs
        "custompatches/additive_voice.c"
        "custompatches/additive_presets.c")
endif()

# DEV menu: temporary controls / diagnostics (CONFIG_SYNTH_DEV_MENU); all
# reference sites are guarded by #if CONFIG_SYNTH_DEV_MENU.
set(dev_srcs)
if(CONFIG_SYNTH_DEV_MENU)
    list(APPEND dev_srcs
        "synth_ui/ui_screen_dev.c"
        "synth_ui/ui_dev_riff.c"
        "sequencer_core/seq_core_dump.c")
endif()

idf_component_register(SRCS "live_play.c" "amy_helpers.c" "amy_fx.c" "fx_bus.c" "arp_core.c" "quantizer.c" "prog_gen.c" "voice_config.c"
                            "filter_scope.c"
                            "synth_ui/synth_ui_state.c"
                            "synth_ui/synth_ui_task.c"
                            "synth_ui/synth_ui_hint.c"
                            "synth_ui/ui_view_resolve.c"
                            "synth_ui/ui_editors.c"
                            "synth_ui/eg_shape.c"
                            "synth_ui/ui_patch_cycle.c"
                            "synth_ui/ui_screen_menu.c"
                            "synth_ui/ui_screen_fxmenu.c"
                            "synth_ui/ui_screen_layermenu.c"
                            "synth_ui/ui_screen_projects.c"
                            "synth_ui/ui_screen_arp.c"
                            "synth_ui/ui_screen_drone.c"
                            "synth_ui/ui_screen_drone_std.c"
                            "synth_ui/ui_screen_prog.c"
                            "synth_ui/ui_screen_chords.c"
                            "synth_ui/ui_screen_bounce.c"
                            "synth_ui/ui_screen_proggen.c"
                            "synth_ui/ui_screen_wireless.c"
                            "synth_ui/ui_screen_stepedit.c"
                            "sequencer_core/seq_core_state.c"
                            "sequencer_core/seq_core_engine.c"
                            "sequencer_core/seq_core_synth.c"
                            "sequencer_core/seq_core_editors.c"
                            "sequencer_core/seq_core_progression.c"
                            "sequencer_core/seq_core_tempo.c"
                            "sequencer_core/seq_core_trig.c"
                            "sequencer_core/seq_trig_pump.c"
                            "sequencer_core/seq_core_snapshot.c"
                            "sequencer_core/seq_chords.c"
                            "custompatches/drone_core.c"
                            "custompatches/drone_std_core.c"
                            "custompatches/bass_presets.c"
                            "custompatches/sample_rec.c"
                            "custompatches/clip_bounce.c"
                            "custompatches/clip_player.c"
                            "custompatches/drum_cache.c"
                            "custompatches/wavetable_bank.c"
                            "project/project_snapshot.c"
                            ${fm_srcs}
                            ${additive_srcs}
                            ${dev_srcs}
                       INCLUDE_DIRS "include" "sequencer_core" "project"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES u8g2 display amy
                       PRIV_REQUIRES seq_clamp project_store usb_audio wireless diagnostics esp_app_format esp_partition)

# Built-in project templates: gen_templates.py checks its codec against the
# firmware sources it reads (fails the build on drift), then writes every
# template spec as an embedded .amp image (project_templates.h).
if(CONFIG_SYNTH_PROJECT_STORE)
    idf_build_get_property(python PYTHON)
    set(tpl_dir "${CMAKE_CURRENT_LIST_DIR}/project/templates")
    set(tpl_out "${CMAKE_CURRENT_BINARY_DIR}/project_templates_data.c")
    file(GLOB tpl_py CONFIGURE_DEPENDS "${tpl_dir}/*.py")
    set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/..")
    add_custom_command(
        OUTPUT "${tpl_out}"
        COMMAND ${python} gen_templates.py --check-src "${comp_dir}" --out "${tpl_out}"
        DEPENDS ${tpl_py}
                "${CMAKE_CURRENT_LIST_DIR}/project/project_snapshot.c"
                "${comp_dir}/project_store/include/project_store.h"
                "${CMAKE_CURRENT_LIST_DIR}/sequencer_core/seq_model.h"
                "${CMAKE_CURRENT_LIST_DIR}/sequencer_core/seq_core_config.h"
                "${CMAKE_CURRENT_LIST_DIR}/include/sequencer_core.h"
                "${CMAKE_CURRENT_LIST_DIR}/include/fx_bus.h"
                "${CMAKE_CURRENT_LIST_DIR}/include/amy_fx.h"
                "${CMAKE_CURRENT_LIST_DIR}/include/arp_core.h"
                "${CMAKE_CURRENT_LIST_DIR}/include/seq_chords.h"
                "${CMAKE_CURRENT_LIST_DIR}/include/synth_slots.h"
                "${CMAKE_CURRENT_LIST_DIR}/include/custompatches/drone_core.h"
                "${comp_dir}/display/chord_types.h"
                "${comp_dir}/amy/src/amy.h"
        WORKING_DIRECTORY "${tpl_dir}"
        VERBATIM)
    target_sources(${COMPONENT_LIB} PRIVATE "${tpl_out}" "${CMAKE_CURRENT_LIST_DIR}/project/project_templates.c")
endif()
# -Wstack-usage is GCC-only; clang rejects the unknown warning option under -Werror.
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(${COMPONENT_LIB} PRIVATE "-Wstack-usage=1024")
endif()
set_source_files_properties(
    "${CMAKE_CURRENT_LIST_DIR}/synth_ui/synth_ui_state.c"
    "${CMAKE_CURRENT_LIST_DIR}/synth_ui/synth_ui_task.c"
    "${CMAKE_CURRENT_LIST_DIR}/synth_ui/ui_view_resolve.c"
    "${CMAKE_CURRENT_LIST_DIR}/synth_ui/ui_editors.c"
    "${CMAKE_CURRENT_LIST_DIR}/synth_ui/ui_patch_cycle.c"
    "${CMAKE_CURRENT_LIST_DIR}/synth_ui/ui_screen_menu.c"
    "${CMAKE_CURRENT_LIST_DIR}/synth_ui/ui_screen_fxmenu.c"
    "${CMAKE_CURRENT_LIST_DIR}/synth_ui/ui_screen_projects.c"
    "${CMAKE_CURRENT_LIST_DIR}/synth_ui/ui_screen_wireless.c"
    "${CMAKE_CURRENT_LIST_DIR}/synth_ui/ui_screen_arp.c"
    "${CMAKE_CURRENT_LIST_DIR}/synth_ui/ui_screen_drone.c"
    "${CMAKE_CURRENT_LIST_DIR}/synth_ui/ui_screen_drone_std.c"
    "${CMAKE_CURRENT_LIST_DIR}/synth_ui/ui_screen_prog.c"
    "${CMAKE_CURRENT_LIST_DIR}/synth_ui/ui_screen_layermenu.c"
    "${CMAKE_CURRENT_LIST_DIR}/synth_ui/ui_screen_stepedit.c"
    "${CMAKE_CURRENT_LIST_DIR}/synth_ui/ui_screen_fm.c"
    PROPERTIES COMPILE_OPTIONS "-Os"
)
set_source_files_properties(
    "${CMAKE_CURRENT_LIST_DIR}/sequencer_core/seq_core_state.c"
    "${CMAKE_CURRENT_LIST_DIR}/sequencer_core/seq_core_engine.c"
    "${CMAKE_CURRENT_LIST_DIR}/sequencer_core/seq_core_synth.c"
    "${CMAKE_CURRENT_LIST_DIR}/sequencer_core/seq_core_editors.c"
    "${CMAKE_CURRENT_LIST_DIR}/sequencer_core/seq_core_progression.c"
    "${CMAKE_CURRENT_LIST_DIR}/sequencer_core/seq_core_tempo.c"
    "${CMAKE_CURRENT_LIST_DIR}/sequencer_core/seq_core_trig.c"
    PROPERTIES COMPILE_OPTIONS "-O2"
)
