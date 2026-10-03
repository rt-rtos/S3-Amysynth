idf_component_register(
    SRCS "display_prog.c" "display_lfo.c" "display_dist.c" "patch_names.c" "priv_i2c_u8g2.c" "display_seq.c" "graph_popup.c"
         "display_menu.c" "display_arp.c" "display_drone.c" "filter_graph.c" "display_stepedit.c" "display_hint.c" "display_badge.c"
         "display_dev.c" "display_env.c" "display_fm.c" "display_wt.c" "display_flush.c" "display_flush_runs.c"
    INCLUDE_DIRS "."
    # synth_core provides seq_model.h (engine-owned data model consumed by the
    # renderer). IDF resolves the resulting synth_core <-> display requirement
    # cycle; a header-only seq_model component would remove it (spec-08 Option B).
    REQUIRES esp_driver_i2c esp_driver_gpio u8g2 synth_core
    PRIV_REQUIRES seq_clamp
)
# -Wstack-usage is GCC-only; clang rejects the unknown warning option under -Werror.
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(${COMPONENT_LIB} PRIVATE "-Wstack-usage=1024")
endif()