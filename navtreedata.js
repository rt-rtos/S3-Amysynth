/*
 @licstart  The following is the entire license notice for the JavaScript code in this file.

 The MIT License (MIT)

 Copyright (C) 1997-2020 by Dimitri van Heesch

 Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 and associated documentation files (the "Software"), to deal in the Software without restriction,
 including without limitation the rights to use, copy, modify, merge, publish, distribute,
 sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in all copies or
 substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
 BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

 @licend  The above is the entire license notice for the JavaScript code in this file
*/
var NAVTREE =
[
  [ "S3-Amysynth", "index.html", [
    [ "Prototype Video", "index.html#autotoc_md0", null ],
    [ "What it does", "index.html#autotoc_md1", null ],
    [ "Hardware", "index.html#autotoc_md2", null ],
    [ "Software", "index.html#autotoc_md3", null ],
    [ "Architecture notes", "index.html#autotoc_md4", [
      [ "Project storage", "index.html#autotoc_md5", null ]
    ] ],
    [ "Optimization & performance", "index.html#autotoc_md6", null ],
    [ "Diagnostics", "index.html#autotoc_md7", [
      [ "Always on - render heartbeat", "index.html#autotoc_md8", null ],
      [ "CONFIG_USB_AUDIO_DIAGNOSTICS - ring-buffer detail", "index.html#autotoc_md9", null ],
      [ "CONFIG_AMYSYNTH_RTOS_STATS - task & core profiling", "index.html#autotoc_md10", null ],
      [ "CONFIG_AMYSYNTH_HEAP_CHECK - heap-corruption bisection", "index.html#autotoc_md11", null ],
      [ "Performance implications of leaving diagnostics on", "index.html#autotoc_md12", null ]
    ] ],
    [ "Usage Guide", "index.html#autotoc_md13", [
      [ "Controls", "index.html#autotoc_md14", null ],
      [ "Screens and navigation", "index.html#autotoc_md16", null ],
      [ "Sequencer", "index.html#autotoc_md18", null ],
      [ "Menu - runtime parameters", "index.html#autotoc_md20", null ],
      [ "Effects page (menu → FX)", "index.html#autotoc_md22", null ],
      [ "Resampler (menu → Sample)", "index.html#autotoc_md24", null ],
      [ "Arpeggiator", "index.html#autotoc_md26", null ],
      [ "Drone synth", "index.html#autotoc_md28", null ],
      [ "Chord progression (Prog screen)", "index.html#autotoc_md30", null ],
      [ "Envelope (ADSR) editor", "index.html#autotoc_md32", null ],
      [ "Filter and LFO editors", "index.html#autotoc_md34", null ],
      [ "Non-obvious quirks", "index.html#autotoc_md36", null ]
    ] ],
    [ "Documentation", "index.html#autotoc_md38", null ],
    [ "Building", "index.html#autotoc_md39", null ],
    [ "Project context", "index.html#autotoc_md40", null ],
    [ "display — display layer", "md_components_2display_2README.html", [
      [ "What it provides", "md_components_2display_2README.html#autotoc_md42", null ],
      [ "Files", "md_components_2display_2README.html#autotoc_md43", null ],
      [ "Design", "md_components_2display_2README.html#autotoc_md44", null ],
      [ "Filter editor (filter_graph)", "md_components_2display_2README.html#autotoc_md45", [
        [ "UI overview", "md_components_2display_2README.html#autotoc_md46", null ],
        [ "Cursor cycle and controls", "md_components_2display_2README.html#autotoc_md47", null ],
        [ "filter_graph_t — data struct", "md_components_2display_2README.html#autotoc_md48", null ],
        [ "Response model", "md_components_2display_2README.html#autotoc_md49", null ],
        [ "Constants", "md_components_2display_2README.html#autotoc_md50", null ]
      ] ],
      [ "HAL quick usage", "md_components_2display_2README.html#autotoc_md51", null ],
      [ "Notes", "md_components_2display_2README.html#autotoc_md52", null ]
    ] ],
    [ "pie_dsp", "md_components_2pie__dsp_2README.html", [
      [ "Why only a memset and a memcpy", "md_components_2pie__dsp_2README.html#autotoc_md54", null ],
      [ "Alignment", "md_components_2pie__dsp_2README.html#autotoc_md55", null ],
      [ "Inline-asm alternative (include/pie_dsp_inline.h, not wired up)", "md_components_2pie__dsp_2README.html#autotoc_md56", null ],
      [ "Provenance", "md_components_2pie__dsp_2README.html#autotoc_md57", null ],
      [ "Users", "md_components_2pie__dsp_2README.html#autotoc_md58", null ]
    ] ],
    [ "Rotary Encoder Component", "md_components_2rotary__encoder_2README.html", [
      [ "Features", "md_components_2rotary__encoder_2README.html#autotoc_md60", null ],
      [ "Primary API", "md_components_2rotary__encoder_2README.html#autotoc_md61", null ]
    ] ],
    [ "Standalone Arpeggiator Architecture", "md_components_2synth__core_2ARP-ARCHITECTURE.html", [
      [ "</blockquote>", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md63", null ],
      [ "Overview", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md64", null ],
      [ "Architecture", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md65", [
        [ "Emit path", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md66", null ]
      ] ],
      [ "Data Model", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md68", [
        [ "arp_state_t  (arp_core.c, file-static s_arp)", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md69", null ],
        [ "arp_view_t  (display_arp.h)", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md70", null ],
        [ "Compile-time limits", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md71", null ]
      ] ],
      [ "AMY Scheduling", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md73", [
        [ "Tag window", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md74", null ],
        [ "Rate → ticks", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md75", null ],
        [ "Sequence computation (arp_core_refresh)", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md76", null ]
      ] ],
      [ "Sources: PATCH vs WAVE", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md78", null ],
      [ "AMY Synth Slot", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md80", null ],
      [ "Refresh Coalescing (performance)", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md82", null ],
      [ "</blockquote>", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md83", null ],
      [ "Public API", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md84", [
        [ "arp_core.h", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md85", null ],
        [ "sequencer_core.h (AMY bridge)", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md86", null ]
      ] ],
      [ "Screen, Input, and Isolation", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md88", [
        [ "Screen selection", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md89", null ],
        [ "Arp screen input", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md90", null ],
        [ "Seq/Arp isolation (main.c)", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md91", null ]
      ] ],
      [ "OLED Display Layout (display_arp.c)", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md93", null ],
      [ "Boot Defaults (Kconfig, arp_core_init)", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md95", null ],
      [ "Future Development Considerations", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md97", null ]
    ] ],
    [ "Stutter House Drone — Architecture", "md_components_2synth__core_2custompatches_2DRONE.html", [
      [ "Files", "md_components_2synth__core_2custompatches_2DRONE.html#autotoc_md99", null ],
      [ "Signal flow", "md_components_2synth__core_2custompatches_2DRONE.html#autotoc_md100", null ],
      [ "AMY voice model (WAVE mode)", "md_components_2synth__core_2custompatches_2DRONE.html#autotoc_md101", [
        [ "How the amplitude / stutter math actually works", "md_components_2synth__core_2custompatches_2DRONE.html#autotoc_md102", null ]
      ] ],
      [ "Chords", "md_components_2synth__core_2custompatches_2DRONE.html#autotoc_md103", null ],
      [ "Tempo sync", "md_components_2synth__core_2custompatches_2DRONE.html#autotoc_md104", null ],
      [ "ADSR envelope (shared graph editor)", "md_components_2synth__core_2custompatches_2DRONE.html#autotoc_md105", null ],
      [ "PATCH mode", "md_components_2synth__core_2custompatches_2DRONE.html#autotoc_md106", null ],
      [ "Synth slots & tag budget", "md_components_2synth__core_2custompatches_2DRONE.html#autotoc_md107", null ],
      [ "Concurrency / safety", "md_components_2synth__core_2custompatches_2DRONE.html#autotoc_md108", null ],
      [ "Input map (drone screen)", "md_components_2synth__core_2custompatches_2DRONE.html#autotoc_md109", null ],
      [ "Screen layout (parameter list)", "md_components_2synth__core_2custompatches_2DRONE.html#autotoc_md110", null ],
      [ "Global FX (related — lives in the menu's FX page, not the drone)", "md_components_2synth__core_2custompatches_2DRONE.html#autotoc_md111", null ]
    ] ],
    [ "synth_core Component", "md_components_2synth__core_2README.html", [
      [ "Layout", "md_components_2synth__core_2README.html#autotoc_md113", null ],
      [ "Highlights", "md_components_2synth__core_2README.html#autotoc_md114", null ],
      [ "Initialization", "md_components_2synth__core_2README.html#autotoc_md115", null ],
      [ "Melodic Envelope System", "md_components_2synth__core_2README.html#autotoc_md116", [
        [ "Data flow", "md_components_2synth__core_2README.html#autotoc_md117", null ],
        [ "Deferred authority over patches", "md_components_2synth__core_2README.html#autotoc_md118", null ],
        [ "Kconfig options", "md_components_2synth__core_2README.html#autotoc_md119", null ],
        [ "Graph editor (graph_popup_amy.c, synth_ui/ui_editors.c)", "md_components_2synth__core_2README.html#autotoc_md120", null ],
        [ "Voice sizing", "md_components_2synth__core_2README.html#autotoc_md121", null ]
      ] ],
      [ "Scale Table", "md_components_2synth__core_2README.html#autotoc_md122", null ]
    ] ],
    [ "software_lfo", "md_components_2synth__core_2sequencer__core_2software__lfo.html", null ],
    [ "USB Audio Component", "md_components_2usb__audio_2README.html", [
      [ "Features", "md_components_2usb__audio_2README.html#autotoc_md124", null ],
      [ "Architecture", "md_components_2usb__audio_2README.html#autotoc_md125", null ],
      [ "Public API (usb_audio.h)", "md_components_2usb__audio_2README.html#autotoc_md126", null ],
      [ "How the project drives it", "md_components_2usb__audio_2README.html#autotoc_md127", null ],
      [ "Configuration", "md_components_2usb__audio_2README.html#autotoc_md128", null ]
    ] ],
    [ "Data Structures", "annotated.html", [
      [ "Data Structures", "annotated.html", "annotated_dup" ],
      [ "Data Structure Index", "classes.html", null ],
      [ "Data Fields", "functions.html", [
        [ "All", "functions.html", "functions_dup" ],
        [ "Variables", "functions_vars.html", "functions_vars" ]
      ] ]
    ] ],
    [ "Files", "files.html", [
      [ "File List", "files.html", "files_dup" ],
      [ "Globals", "globals.html", [
        [ "All", "globals.html", "globals_dup" ],
        [ "Functions", "globals_func.html", "globals_func" ],
        [ "Variables", "globals_vars.html", "globals_vars" ],
        [ "Typedefs", "globals_type.html", null ],
        [ "Enumerations", "globals_enum.html", null ],
        [ "Enumerator", "globals_eval.html", "globals_eval" ],
        [ "Macros", "globals_defs.html", "globals_defs" ]
      ] ]
    ] ]
  ] ]
];

var NAVTREEINDEX =
[
"additive__presets_8c.html",
"amy_8h.html#a3bce2ee8300ba2cae1a86b44ae9282bc",
"amy_8h.html#aa82684d023520c90157f420af1892550",
"amy__midi_8c.html#a62b2b9864ea04138e178f5ccf183badf",
"chord__types_8h.html#a3795d68443e9e64d7296904a3e1c7ad5a07deb7609146a64349df543e218ec1d5",
"display__trackopts_8c.html#a3359efcbb5299f044a07dbe2e5fd4bf0",
"drone__std__core_8h.html#a109714126b434c5e40eedbea382d1c36",
"globals_eval_w.html",
"interp__partials_8c.html#a37a3a081cd7f9a264ee2b960c8e7ce20",
"oscillators_8c.html#ac9a4e5097f6a93dfaa5cadcc21821caf",
"project__fs_8h.html#afa46291d7a1dbec741063c113b4223b3",
"seq__core__config_8h.html#ab603c6405a65dd1d66149eb0d55cc123",
"seq__core__tempo_8c.html#a86d8f7c002ff7e7037af82f620e502ff",
"sequencer__core_8h.html#aacff3fe360aea5879018471f9089cfde",
"structcustom__oscillator.html#af11d184d20f6616a7f8b6cc79dcef91d",
"structmemorypcm__ll__t.html#a48b6fd0a8ffe137d957d575b3c41a275",
"structstepedit__view__t.html#ac28e7e065d04acf11323d2fa4e44d249",
"synth__ui__internal_8h.html#a8f295b591386f02634765f490c7a55f7",
"ui__patch__cycle_8c.html#a5a85b9c772bbeb480b209a3e6ea92b4c",
"ui__view__resolve_8c.html#a44b763d125a8138a1d15f0e63a043947"
];

var SYNCONMSG = 'click to disable panel synchronisation';
var SYNCOFFMSG = 'click to enable panel synchronisation';