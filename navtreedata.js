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
      [ "Always on - status LED", "index.html#autotoc_md9", null ],
      [ "CONFIG_USB_AUDIO_DIAGNOSTICS - ring-buffer detail", "index.html#autotoc_md10", null ],
      [ "CONFIG_AMYSYNTH_RTOS_STATS - task & core profiling", "index.html#autotoc_md11", null ],
      [ "CONFIG_AMYSYNTH_HEAP_CHECK - heap-corruption bisection", "index.html#autotoc_md12", null ],
      [ "Performance implications of leaving diagnostics on", "index.html#autotoc_md13", null ]
    ] ],
    [ "Usage Guide", "index.html#autotoc_md14", [
      [ "Controls", "index.html#autotoc_md15", null ],
      [ "Screens and navigation", "index.html#autotoc_md17", null ],
      [ "Sequencer", "index.html#autotoc_md19", null ],
      [ "Menu - runtime parameters", "index.html#autotoc_md21", null ],
      [ "Effects page (menu → FX)", "index.html#autotoc_md23", null ],
      [ "Chord presets (menu → Chords)", "index.html#autotoc_md25", null ],
      [ "Projects (menu → Projects)", "index.html#autotoc_md27", null ],
      [ "Wireless - BLE MIDI in (menu → Wireless)", "index.html#autotoc_md29", null ],
      [ "Resampler (menu → Sample)", "index.html#autotoc_md31", null ],
      [ "Arpeggiator", "index.html#autotoc_md33", null ],
      [ "Drones", "index.html#autotoc_md35", null ],
      [ "Chord progression (Prog screen)", "index.html#autotoc_md37", null ],
      [ "Non-obvious quirks", "index.html#autotoc_md39", null ]
    ] ],
    [ "Voice editors", "index.html#autotoc_md41", [
      [ "Envelope (ADSR) editor", "index.html#autotoc_md42", null ],
      [ "Filter editor", "index.html#autotoc_md44", null ],
      [ "LFO editor", "index.html#autotoc_md46", null ],
      [ "Distortion editor", "index.html#autotoc_md48", null ]
    ] ],
    [ "FM operator editor", "index.html#autotoc_md49", [
      [ "Screen", "index.html#autotoc_md50", null ],
      [ "Controls", "index.html#autotoc_md51", null ],
      [ "Operator envelopes", "index.html#autotoc_md52", null ],
      [ "Custom topologies and their limits", "index.html#autotoc_md53", null ]
    ] ],
    [ "Documentation", "index.html#autotoc_md55", null ],
    [ "Building", "index.html#autotoc_md56", null ],
    [ "Project context", "index.html#autotoc_md57", null ],
    [ "display — display layer", "md_components_2display_2README.html", [
      [ "What it provides", "md_components_2display_2README.html#autotoc_md59", null ],
      [ "Files", "md_components_2display_2README.html#autotoc_md60", null ],
      [ "Design", "md_components_2display_2README.html#autotoc_md61", null ],
      [ "Filter editor (filter_graph)", "md_components_2display_2README.html#autotoc_md62", [
        [ "UI overview", "md_components_2display_2README.html#autotoc_md63", null ],
        [ "Cursor cycle and controls", "md_components_2display_2README.html#autotoc_md64", null ],
        [ "filter_graph_t — data struct", "md_components_2display_2README.html#autotoc_md65", null ],
        [ "Response model", "md_components_2display_2README.html#autotoc_md66", null ],
        [ "Constants", "md_components_2display_2README.html#autotoc_md67", null ]
      ] ],
      [ "HAL quick usage", "md_components_2display_2README.html#autotoc_md68", null ],
      [ "Notes", "md_components_2display_2README.html#autotoc_md69", null ]
    ] ],
    [ "Rotary Encoder Component", "md_components_2rotary__encoder_2README.html", [
      [ "Features", "md_components_2rotary__encoder_2README.html#autotoc_md71", null ],
      [ "Primary API", "md_components_2rotary__encoder_2README.html#autotoc_md72", null ]
    ] ],
    [ "status_led Component", "md_components_2status__led_2README.html", [
      [ "Features", "md_components_2status__led_2README.html#autotoc_md74", null ],
      [ "Architecture", "md_components_2status__led_2README.html#autotoc_md75", [
        [ "Load measurement", "md_components_2status__led_2README.html#autotoc_md76", null ],
        [ "Contract", "md_components_2status__led_2README.html#autotoc_md77", null ]
      ] ],
      [ "Public API (status_led.h)", "md_components_2status__led_2README.html#autotoc_md78", null ],
      [ "Configuration", "md_components_2status__led_2README.html#autotoc_md79", null ],
      [ "Notes", "md_components_2status__led_2README.html#autotoc_md80", null ]
    ] ],
    [ "Standalone Arpeggiator Architecture", "md_components_2synth__core_2ARP-ARCHITECTURE.html", [
      [ "</blockquote>", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md82", null ],
      [ "Overview", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md83", null ],
      [ "Architecture", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md84", [
        [ "Emit path", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md85", null ]
      ] ],
      [ "Data Model", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md87", [
        [ "arp_state_t  (arp_core.c, file-static s_arp)", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md88", null ],
        [ "arp_view_t  (display_arp.h)", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md89", null ],
        [ "Compile-time limits", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md90", null ]
      ] ],
      [ "AMY Scheduling", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md92", [
        [ "Tag window", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md93", null ],
        [ "Rate → ticks", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md94", null ],
        [ "Sequence computation (arp_core_refresh)", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md95", null ]
      ] ],
      [ "Sound selection", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md97", null ],
      [ "AMY Synth Slot", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md99", null ],
      [ "Refresh Coalescing (performance)", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md101", null ],
      [ "</blockquote>", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md102", null ],
      [ "Public API", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md103", [
        [ "arp_core.h", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md104", null ],
        [ "sequencer_core.h (AMY bridge)", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md105", null ]
      ] ],
      [ "Screen, Input, and Isolation", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md107", [
        [ "Screen selection", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md108", null ],
        [ "Arp screen input", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md109", null ],
        [ "Seq/Arp isolation (main.c)", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md110", null ]
      ] ],
      [ "OLED Display Layout (display_arp.c)", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md112", null ],
      [ "Boot Defaults (Kconfig, arp_core_init)", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md114", null ],
      [ "Future Development Considerations", "md_components_2synth__core_2ARP-ARCHITECTURE.html#autotoc_md116", null ]
    ] ],
    [ "Stutter House Drone — Architecture", "md_components_2synth__core_2custompatches_2DRONE.html", [
      [ "Files", "md_components_2synth__core_2custompatches_2DRONE.html#autotoc_md118", [
        [ "The sibling: the normal (free-running) drone", "md_components_2synth__core_2custompatches_2DRONE.html#autotoc_md119", null ]
      ] ],
      [ "Signal flow", "md_components_2synth__core_2custompatches_2DRONE.html#autotoc_md120", null ],
      [ "AMY voice model (WAVE mode)", "md_components_2synth__core_2custompatches_2DRONE.html#autotoc_md121", [
        [ "How the amplitude / stutter math actually works", "md_components_2synth__core_2custompatches_2DRONE.html#autotoc_md122", null ]
      ] ],
      [ "Chords", "md_components_2synth__core_2custompatches_2DRONE.html#autotoc_md123", null ],
      [ "Tempo sync", "md_components_2synth__core_2custompatches_2DRONE.html#autotoc_md124", null ],
      [ "ADSR envelope (shared graph editor)", "md_components_2synth__core_2custompatches_2DRONE.html#autotoc_md125", null ],
      [ "PATCH mode", "md_components_2synth__core_2custompatches_2DRONE.html#autotoc_md126", null ],
      [ "Synth slots & tag budget", "md_components_2synth__core_2custompatches_2DRONE.html#autotoc_md127", null ],
      [ "Concurrency / safety", "md_components_2synth__core_2custompatches_2DRONE.html#autotoc_md128", null ],
      [ "Input map (drone screen)", "md_components_2synth__core_2custompatches_2DRONE.html#autotoc_md129", null ],
      [ "Screen layout (parameter list)", "md_components_2synth__core_2custompatches_2DRONE.html#autotoc_md130", null ],
      [ "Global FX (related — lives in the menu's FX page, not the drone)", "md_components_2synth__core_2custompatches_2DRONE.html#autotoc_md131", null ]
    ] ],
    [ "synth_core Component", "md_components_2synth__core_2README.html", [
      [ "Layout", "md_components_2synth__core_2README.html#autotoc_md133", null ],
      [ "Highlights", "md_components_2synth__core_2README.html#autotoc_md134", null ],
      [ "Initialization", "md_components_2synth__core_2README.html#autotoc_md135", null ],
      [ "Melodic Envelope System", "md_components_2synth__core_2README.html#autotoc_md136", [
        [ "Data flow", "md_components_2synth__core_2README.html#autotoc_md137", null ],
        [ "Deferred authority over patches", "md_components_2synth__core_2README.html#autotoc_md138", null ],
        [ "Kconfig options", "md_components_2synth__core_2README.html#autotoc_md139", null ],
        [ "Graph editor (graph_popup_amy.c, synth_ui/ui_editors.c)", "md_components_2synth__core_2README.html#autotoc_md140", null ],
        [ "Voice sizing", "md_components_2synth__core_2README.html#autotoc_md141", null ]
      ] ],
      [ "Scale Table", "md_components_2synth__core_2README.html#autotoc_md142", null ]
    ] ],
    [ "software_lfo", "md_components_2synth__core_2sequencer__core_2software__lfo.html", null ],
    [ "USB Audio Component", "md_components_2usb__audio_2README.html", [
      [ "Features", "md_components_2usb__audio_2README.html#autotoc_md144", null ],
      [ "Architecture", "md_components_2usb__audio_2README.html#autotoc_md145", [
        [ "Dropout counters", "md_components_2usb__audio_2README.html#autotoc_md146", null ]
      ] ],
      [ "Public API (usb_audio.h)", "md_components_2usb__audio_2README.html#autotoc_md147", null ],
      [ "How the project drives it", "md_components_2usb__audio_2README.html#autotoc_md148", null ],
      [ "Configuration", "md_components_2usb__audio_2README.html#autotoc_md149", null ]
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
"amy_8h.html#a2a16530c6ae6f2cb0269851d1f153522",
"amy_8h.html#a81433457d5e081484712a6d6c4ef6745",
"amy_8h.html#afbbf29ae1f21ea98ef85842c6f80ff86",
"api_8c.html#aea2ccbcb73cd3c4f537711d6ec02d2bb",
"cv__trigger_8c.html#a63ec909accd9a1f6689974f626da91ff",
"display__lfo_8c.html#a0383f936798e82915b00e03f78c845f3",
"drone__core_8h.html#a988f88ab45a5515f5764b6fa0dcda310",
"filters_8c.html#ac0aef8fdc7804fc38562c5dee84b3854",
"globals_vars_f.html",
"interp__partials_8h.html#a4b8c4d6c7a2b47e1c72c0f26e5e2374b",
"oscillators_8c.html#a249d4060fb10a83cd62af444ba8562d5",
"pcm__gamma808_8h.html#a308a6ddcb882cd84b2f474f18e44195e",
"render__clock_8c.html#afd737851dbf251a1469330e9ea7fbe95",
"seq__core__engine_8c.html#a320c1c16d81ff34ba69ebdff53cbb2ee",
"seq__core__synth_8c_source.html",
"sequencer__core_8h.html#a3085d901faa5af04143521cbdaf739bc",
"structamy__event.html#a33baa52db558fffcdcf9a1c8bd85cf39",
"structdrone__vis__t.html#a60d3dba5e98728c65e0d85dc143a1a1d",
"structmidi__mapping.html#a7528c3e9ba093132df01b28a35122ef8",
"structstaged__drone__t.html#af1687ecc2867043ace733afaec0108f9",
"synth__ui_8h.html#a950a10880d1a506203286cc9dd269407",
"ui__editors_8c.html#a1bc1e03a7eb3fdc6da725512455c0f3e",
"ui__screen__drone__std_8c.html#a04c995a9732d4ef6e88b5e9f37ceaa91aa01c3c945f0f29a95110ac8601215433",
"usb__audio_8c.html#aa4178cbacc862cdd2c407da69c26a457"
];

var SYNCONMSG = 'click to disable panel synchronisation';
var SYNCOFFMSG = 'click to enable panel synchronisation';