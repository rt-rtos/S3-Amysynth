/*
 * patch_names — human-readable names for the AMY built-in patch banks.
 *
 * Names transcribed from components/amy/src/patches.h (patch_commands[] table
 * comments). Gated by CONFIG_SEQ_PATCH_SHOW_NAMES so it costs zero flash when
 * names are not wanted.
 *
 *   0..127  : Roland Juno-106 presets
 *   128..255: Yamaha DX7 presets
 *   256     : AMY built-in heterodyne piano
 */

#include "patch_names.h"
#include "custompatches/wavetable_bank.h"

#if CONFIG_SEQ_PATCH_SHOW_NAMES

/* This table is positional: the 267-271 slot is always present (either real
 * wavetable names or "(reserved)" placeholders, see below) so that FM/ALGO at
 * 272-276 lands at a fixed index regardless of CONFIG_AMY_WAVETABLE. */
#define SEQ_PATCH_NAME_COUNT 280  /* 0..279 inclusive (257-263 = waves, 264-266 = bass
                                     presets, 267-271 = wavetable banks or reserved,
                                     272-276 = FM/ALGO, 277-279 = additive) */

static const char *const s_patch_names[SEQ_PATCH_NAME_COUNT] = {
    /* ── Juno-106 (0..127) ── */
    /*   0 */ "Juno Brass Set 1",
    /*   1 */ "Juno Brass Swell",
    /*   2 */ "Juno Trumpet",
    /*   3 */ "Juno Flutes",
    /*   4 */ "Juno Moving Strings",
    /*   5 */ "Juno Brass & Strings",
    /*   6 */ "Juno Choir",
    /*   7 */ "Juno Piano I",
    /*   8 */ "Juno Organ I",
    /*   9 */ "Juno Organ II",
    /*  10 */ "Juno Combo Organ",
    /*  11 */ "Juno Calliope",
    /*  12 */ "Juno Donald Pluck",
    /*  13 */ "Juno Celeste",
    /*  14 */ "Juno E.Piano I",
    /*  15 */ "Juno E.Piano II",
    /*  16 */ "Juno Clock Chimes",
    /*  17 */ "Juno Steel Drums",
    /*  18 */ "Juno Xylophone",
    /*  19 */ "Juno Brass III",
    /*  20 */ "Juno Fanfare",
    /*  21 */ "Juno String III",
    /*  22 */ "Juno Pizzicato",
    /*  23 */ "Juno High Strings",
    /*  24 */ "Juno Bass Clarinet",
    /*  25 */ "Juno English Horn",
    /*  26 */ "Juno Brass Ensemble",
    /*  27 */ "Juno Guitar",
    /*  28 */ "Juno Koto",
    /*  29 */ "Juno Dark Pluck",
    /*  30 */ "Juno Funky I",
    /*  31 */ "Juno Synth Bass I",
    /*  32 */ "Juno Lead I",
    /*  33 */ "Juno Lead II",
    /*  34 */ "Juno Lead III",
    /*  35 */ "Juno Funky II",
    /*  36 */ "Juno Synth Bass II",
    /*  37 */ "Juno Funky III",
    /*  38 */ "Juno Thud Wah",
    /*  39 */ "Juno Going Up",
    /*  40 */ "Juno Piano II",
    /*  41 */ "Juno Clav",
    /*  42 */ "Juno Frontier Organ",
    /*  43 */ "Juno Snare Drum",
    /*  44 */ "Juno Tom Toms",
    /*  45 */ "Juno Timpani",
    /*  46 */ "Juno Shaker",
    /*  47 */ "Juno Synth Pad",
    /*  48 */ "Juno Sweep I",
    /*  49 */ "Juno Pluck Sweep",
    /*  50 */ "Juno Repeater",
    /*  51 */ "Juno Sweep II",
    /*  52 */ "Juno Pluck Bell",
    /*  53 */ "Juno Dark Synth Piano",
    /*  54 */ "Juno Sustainer",
    /*  55 */ "Juno Wah Release",
    /*  56 */ "Juno Gong",
    /*  57 */ "Juno Resonance Funk",
    /*  58 */ "Juno Drum Booms",
    /*  59 */ "Juno Dust Storm",
    /*  60 */ "Juno Rocket Men",
    /*  61 */ "Juno Hand Claps",
    /*  62 */ "Juno FX Sweep",
    /*  63 */ "Juno Caverns",
    /*  64 */ "Juno Strings",
    /*  65 */ "Juno Violin",
    /*  66 */ "Juno Chorus Vibes",
    /*  67 */ "Juno Organ 1",
    /*  68 */ "Juno Harpsichord 1",
    /*  69 */ "Juno Recorder",
    /*  70 */ "Juno Perc. Pluck",
    /*  71 */ "Juno Noise Sweep",
    /*  72 */ "Juno Space Chimes",
    /*  73 */ "Juno Nylon Guitar",
    /*  74 */ "Juno Orchestral Pad",
    /*  75 */ "Juno Bright Pluck",
    /*  76 */ "Juno Organ Bell",
    /*  77 */ "Juno Accordion",
    /*  78 */ "Juno FX Rise 1",
    /*  79 */ "Juno FX Rise 2",
    /*  80 */ "Juno Brass",
    /*  81 */ "Juno Helicopter",
    /*  82 */ "Juno Lute",
    /*  83 */ "Juno Chorus Funk",
    /*  84 */ "Juno Tomita",
    /*  85 */ "Juno FX Sweep 1",
    /*  86 */ "Juno Sharp Reed",
    /*  87 */ "Juno Bass Pluck",
    /*  88 */ "Juno Resonant Rise",
    /*  89 */ "Juno Harpsichord 2",
    /*  90 */ "Juno Dark Ensemble",
    /*  91 */ "Juno Contact Wah",
    /*  92 */ "Juno Noise Sweep 2",
    /*  93 */ "Juno Glassy Wah",
    /*  94 */ "Juno Phase Ensemble",
    /*  95 */ "Juno Chorused Bell",
    /*  96 */ "Juno Clav",
    /*  97 */ "Juno Organ 2",
    /*  98 */ "Juno Bassoon",
    /*  99 */ "Juno Auto Rel Noise Sweep",
    /* 100 */ "Juno Brass Ensemble",
    /* 101 */ "Juno Ethereal",
    /* 102 */ "Juno Chorus Bell 2",
    /* 103 */ "Juno Blizzard",
    /* 104 */ "Juno E.Piano Tremolo",
    /* 105 */ "Juno Clarinet",
    /* 106 */ "Juno Thunder",
    /* 107 */ "Juno Reedy Organ",
    /* 108 */ "Juno Flute / Horn",
    /* 109 */ "Juno Toy Rhodes",
    /* 110 */ "Juno Surf's Up",
    /* 111 */ "Juno OW Bass",
    /* 112 */ "Juno Piccolo",
    /* 113 */ "Juno Melodic Taps",
    /* 114 */ "Juno Meow Brass",
    /* 115 */ "Juno Violin (high)",
    /* 116 */ "Juno High Bells",
    /* 117 */ "Juno Rolling Wah",
    /* 118 */ "Juno Ping Bell",
    /* 119 */ "Juno Brassy Organ",
    /* 120 */ "Juno Low Dark Strings",
    /* 121 */ "Juno Piccolo Trumpet",
    /* 122 */ "Juno Cello",
    /* 123 */ "Juno High Strings",
    /* 124 */ "Juno Rocket Men",
    /* 125 */ "Juno Forbidden Planet",
    /* 126 */ "Juno Froggy",
    /* 127 */ "Juno Owgan",

    /* ── DX7 (128..255) ── */
    /* 128 */ "DX7 Brass 1",
    /* 129 */ "DX7 Brass 2",
    /* 130 */ "DX7 Brass 3",
    /* 131 */ "DX7 Strings 1",
    /* 132 */ "DX7 Strings 2",
    /* 133 */ "DX7 Strings 3",
    /* 134 */ "DX7 Orchestra",
    /* 135 */ "DX7 Piano 1",
    /* 136 */ "DX7 Piano 2",
    /* 137 */ "DX7 Piano 3",
    /* 138 */ "DX7 E.Piano 1",
    /* 139 */ "DX7 Guitar 1",
    /* 140 */ "DX7 Guitar 2",
    /* 141 */ "DX7 Syn-Lead 1",
    /* 142 */ "DX7 Bass 1",
    /* 143 */ "DX7 Bass 2",
    /* 144 */ "DX7 E.Organ 1",
    /* 145 */ "DX7 Pipes 1",
    /* 146 */ "DX7 Harpsich 1",
    /* 147 */ "DX7 Clav 1",
    /* 148 */ "DX7 Vibe 1",
    /* 149 */ "DX7 Marimba",
    /* 150 */ "DX7 Koto",
    /* 151 */ "DX7 Flute 1",
    /* 152 */ "DX7 Orch-Chime",
    /* 153 */ "DX7 Tub Bells",
    /* 154 */ "DX7 Steel Drum",
    /* 155 */ "DX7 Timpani",
    /* 156 */ "DX7 Refs Whistle",
    /* 157 */ "DX7 Voice 1",
    /* 158 */ "DX7 Train",
    /* 159 */ "DX7 Take Off",
    /* 160 */ "DX7 Piano 4",
    /* 161 */ "DX7 Piano 5",
    /* 162 */ "DX7 E.Piano 2",
    /* 163 */ "DX7 E.Piano 3",
    /* 164 */ "DX7 E.Piano 4",
    /* 165 */ "DX7 Piano 5ths",
    /* 166 */ "DX7 Celeste",
    /* 167 */ "DX7 Toy Piano",
    /* 168 */ "DX7 Harpsich 2",
    /* 169 */ "DX7 Harpsich 3",
    /* 170 */ "DX7 Clav 2",
    /* 171 */ "DX7 Clav 3",
    /* 172 */ "DX7 E.Organ 2",
    /* 173 */ "DX7 E.Organ 3",
    /* 174 */ "DX7 E.Organ 4",
    /* 175 */ "DX7 E.Organ 5",
    /* 176 */ "DX7 Pipes 2",
    /* 177 */ "DX7 Pipes 3",
    /* 178 */ "DX7 Pipes 4",
    /* 179 */ "DX7 Caliope",
    /* 180 */ "DX7 Accordion",
    /* 181 */ "DX7 Sitar",
    /* 182 */ "DX7 Guitar 3",
    /* 183 */ "DX7 Guitar 4",
    /* 184 */ "DX7 Guitar 5",
    /* 185 */ "DX7 Guitar 6",
    /* 186 */ "DX7 Lute",
    /* 187 */ "DX7 Banjo",
    /* 188 */ "DX7 Harp 1",
    /* 189 */ "DX7 Harp 2",
    /* 190 */ "DX7 Bass 3",
    /* 191 */ "DX7 Bass 4",
    /* 192 */ "DX7 Piccolo",
    /* 193 */ "DX7 Flute 2",
    /* 194 */ "DX7 Oboe",
    /* 195 */ "DX7 Clarinet",
    /* 196 */ "DX7 Sax BC",
    /* 197 */ "DX7 Bassoon",
    /* 198 */ "DX7 Strings 4",
    /* 199 */ "DX7 Strings 5",
    /* 200 */ "DX7 Strings 6",
    /* 201 */ "DX7 Strings 7",
    /* 202 */ "DX7 Strings 8",
    /* 203 */ "DX7 Brass 4",
    /* 204 */ "DX7 Brass 5",
    /* 205 */ "DX7 Brass 6 BC",
    /* 206 */ "DX7 Brass 7",
    /* 207 */ "DX7 Brass 8",
    /* 208 */ "DX7 Recorder",
    /* 209 */ "DX7 Harmonica 1",
    /* 210 */ "DX7 Harmonica 2 BC",
    /* 211 */ "DX7 Voice 2",
    /* 212 */ "DX7 Voice 3",
    /* 213 */ "DX7 Glockenspiel",
    /* 214 */ "DX7 Vibe 2",
    /* 215 */ "DX7 Xylophone",
    /* 216 */ "DX7 Chimes",
    /* 217 */ "DX7 Gong 1",
    /* 218 */ "DX7 Gong 2",
    /* 219 */ "DX7 Bells",
    /* 220 */ "DX7 Cow Bell",
    /* 221 */ "DX7 Block",
    /* 222 */ "DX7 Flexatone",
    /* 223 */ "DX7 Log Drum",
    /* 224 */ "DX7 Syn-Lead 2",
    /* 225 */ "DX7 Syn-Lead 3",
    /* 226 */ "DX7 Syn-Lead 4",
    /* 227 */ "DX7 Syn-Lead 5",
    /* 228 */ "DX7 Syn-Clav 1",
    /* 229 */ "DX7 Syn-Clav 2",
    /* 230 */ "DX7 Syn-Clav 3",
    /* 231 */ "DX7 Syn-Piano",
    /* 232 */ "DX7 SynBrass 1",
    /* 233 */ "DX7 SynBrass 2",
    /* 234 */ "DX7 SynOrgan 1",
    /* 235 */ "DX7 SynOrgan 2",
    /* 236 */ "DX7 Syn-Vox",
    /* 237 */ "DX7 Syn-Orch",
    /* 238 */ "DX7 Syn-Bass 1",
    /* 239 */ "DX7 Syn-Bass 2",
    /* 240 */ "DX7 Harp-Flute",
    /* 241 */ "DX7 Bell-Flute",
    /* 242 */ "DX7 E.P-Brass BC",
    /* 243 */ "DX7 T.Bell-Expand",
    /* 244 */ "DX7 Chime-String",
    /* 245 */ "DX7 B.Drum-Snare",
    /* 246 */ "DX7 Shimmer",
    /* 247 */ "DX7 Evolution",
    /* 248 */ "DX7 Water Garden",
    /* 249 */ "DX7 Wasp Sting",
    /* 250 */ "DX7 Laser Gun",
    /* 251 */ "DX7 Descent",
    /* 252 */ "DX7 Octave War",
    /* 253 */ "DX7 Grand Prix",
    /* 254 */ "DX7 St.Helens",
    /* 255 */ "DX7 Explosion",

    /* ── Built-in piano (256) ── */
    /* 256 */ "Built-in Piano",

    /* ── Raw waveform patches (257-263) ── */
    /* 257 */ "SINE",
    /* 258 */ "SAW DOWN",
    /* 259 */ "SAW UP",
    /* 260 */ "PULSE",
    /* 261 */ "TRIANGLE",
    /* 262 */ "NOISE",
    /* 263 */ "KS",

    /* ── Multi-osc bass presets (264-266) ── */
    /* 264 */ "Bass: Sub Detune",
    /* 265 */ "Bass: Acid Pluck",
    /* 266 */ "Bass: DX7 Style",

#if CONFIG_AMY_WAVETABLE
    /* ── Wavetable banks (267-271) ── */
    /* 267 */ "Wavetable: 111",
    /* 268 */ "Wavetable: Braids01",
    /* 269 */ "Wavetable: PPG WA00",
    /* 270 */ "Wavetable: Sine2Saw",
    /* 271 */ "Wavetable: Viral",
#else
    /* Positional table, not designated-index: SEQ_PATCH_FM_BASE is fixed at 272
     * (sequencer_core.h), so these placeholders must hold the 267-271 slots
     * CONFIG_AMY_WAVETABLE would otherwise fill. */
    /* 267 */ "(reserved)",
    /* 268 */ "(reserved)",
    /* 269 */ "(reserved)",
    /* 270 */ "(reserved)",
    /* 271 */ "(reserved)",
#endif

    /* ── FM/ALGO 6-op voices (272-276) ── */
    /* 272 */ "FM Bass",
    /* 273 */ "FM E.Piano",
    /* 274 */ "FM Bell",
    /* 275 */ "FM Lead",
    /* 276 */ "FM Custom (edit)",

    /* ── Additive/partials voices (277-279) - named unconditionally so the
     * table stays positional under the CONFIG_SYNTH_ADDITIVE gate. ── */
    /* 277 */ "Add Organ",
    /* 278 */ "Add Bell",
    /* 279 */ "Add Custom",
};

const char *patch_name_for(uint16_t patch)
{
    if (patch >= SEQ_PATCH_NAME_COUNT) {
        /* App wavetable bank: names live with the tables, not in this table. */
        return wavetable_bank_patch_name(patch);
    }
    return s_patch_names[patch];
}

/* ── PCM ROM drum presets ──
 * Positional table matching the compiled-in bank's pcm_map[] (names from the
 * generator comments in pcm_gamma808.h / pcm_tiny.h). Numbering is bank-
 * dependent, hence the CONFIG_AMY_PCM_GAMMA808 split. Wavetable map entries
 * above the drums are unnamed on purpose (not selectable as drums). */
#if CONFIG_AMY_PCM_GAMMA808
#define SEQ_PCM_NAME_COUNT 19
static const char *const s_pcm_preset_names[SEQ_PCM_NAME_COUNT] = {
    /*  0 */ "808 Bass Drum 1",
    /*  1 */ "808 Bass Drum 2",
    /*  2 */ "808 Bass Drum 3",
    /*  3 */ "808 Clap",
    /*  4 */ "808 Clave",
    /*  5 */ "808 Conga Hi",
    /*  6 */ "808 Conga Lo",
    /*  7 */ "808 Conga Mid",
    /*  8 */ "808 Cowbell",
    /*  9 */ "808 HiHat Closed",
    /* 10 */ "808 HiHat Open",
    /* 11 */ "808 Shaker",
    /* 12 */ "808 Snare 1",
    /* 13 */ "808 Snare 2",
    /* 14 */ "808 Snare 3",
    /* 15 */ "808 Rimshot",
    /* 16 */ "808 Tom Lo",
    /* 17 */ "808 Tom Hi",
    /* 18 */ "808 Cymbal",
};
#else
#define SEQ_PCM_NAME_COUNT 11
static const char *const s_pcm_preset_names[SEQ_PCM_NAME_COUNT] = {
    /*  0 */ "808 Maraca",
    /*  1 */ "808 Kick 4",
    /*  2 */ "808 Snare 4",
    /*  3 */ "808 Snare 7",
    /*  4 */ "808 Snare 10",
    /*  5 */ "808 Snare 12",
    /*  6 */ "808 C-Hat 1",
    /*  7 */ "808 O-Hat 1",
    /*  8 */ "808 Tom Lo M",
    /*  9 */ "808 Dry Clap",
    /* 10 */ "808 Cowbell",
};
#endif /* CONFIG_AMY_PCM_GAMMA808 */

#if CONFIG_AMY_PCM_GAMMA808
/* Gamma9001 sample banks (presets 256..391), streamed from the 'drums' flash
 * partition. Order mirrors the vendored amy/src/pcm_gamma9001.h map exactly -
 * re-verify after any AMY re-vendor. Names shortened for the drum banner. */
#define SEQ_GAMMA_NAME_FIRST 256
static const char *const s_gamma_preset_names[] = {
    /* tr909 256-272 */
    "909 BD", "909 BD Lo", "909 Clap", "909 Crash", "909 HH",
    "909 HH Long", "909 HH Short", "909 OH", "909 OH Long", "909 OH Short",
    "909 Ride", "909 Rim", "909 SD", "909 SD Short", "909 Tom Hi",
    "909 Tom Lo", "909 Tom Mid",
    /* linn9000 273-282 */
    "Linn Kick", "Linn Bongo", "Linn Cowbell", "Linn Cymbal", "Linn HH Cl",
    "Linn HH Op", "Linn Rimshot", "Linn Snare", "Linn Tamb", "Linn Tom",
    /* mr12 283-286 */
    "MR12 HH Cl", "MR12 HH Op", "MR12 Kick", "MR12 Snare",
    /* synthetics 287-310 */
    "Syn Metal 09", "Syn Metal 16", "Syn Static 07", "Syn Wood 01",
    "Syn Wood 02", "Syn Wood 03", "Syn BD 03", "Syn BD 04", "Syn BD 07",
    "Tokyo Burst", "Tokyo Deep", "Tokyo Jet", "Tokyo Space", "Syn Hizz 06",
    "Syn Pew 03", "Syn Boink 02", "Syn Boink 04", "Syn Blop 01",
    "Syn Blop 02", "Syn Click 05", "Syn Click 06", "Syn Click 07",
    "Syn Click 08", "Syn Click 09",
    /* power 311-330 */
    "Pwr Real Kick", "Pwr Metal Kick", "Pwr Side Stick", "Pwr Snare",
    "Pwr Hand Clap", "Pwr Gate Snare", "Pwr Tom 1", "Pwr Tight HH",
    "Pwr Tom 2", "Pwr Pedal HH", "Pwr Tom 3", "Pwr Open HH", "Pwr Tom 4",
    "Pwr Tom 5", "Pwr Crash", "Pwr Tom 6", "Pwr Ride Edge", "Pwr Ride Cup",
    "Pwr Splash", "Pwr Cowbell",
    /* percussion 331-374 */
    "Afr Conga 1", "Afr Conga 2", "Afr Conga 3", "Afr Conga 4",
    "Bongo 1", "Bongo 2", "Bongo 3", "Bongo 4", "Bongo 5", "Bongo 6",
    "Bongo 7", "Conga 1", "Conga 2", "Conga 3", "Conga 4", "Conga 5",
    "Conga 6", "Digeridoo", "Kanu", "Noise Kick", "Old Snare", "Shaker 1",
    "Shaker 2", "Sitar", "Strings 1", "Strings 2", "Strings Sweep",
    "Tabla 1", "Tabla 2", "Tabla 3", "Tamb 1", "Tamb 3", "Tamb 4", "Tamp 2",
    "Timbales 1", "Timbales 2", "Timbales 3", "Timbales 4", "Timpani 1",
    "Timpani 2", "Triangle 1", "Triangle 2", "Untitled", "Whistle",
    /* acoustic 375-377 */
    "Ac Tambourine", "Ac Shaker Sh", "Ac Shaker Tiny",
    /* extras 378-391 */
    "JCave", "Laser", "Mach3", "Silver", "Vibrablib", "HeHa", "HiHi",
    "Dim Future", "Smiling", "Ana Strings", "BassRec", "Xox", "Beach",
    "Narrow",
};
#define SEQ_GAMMA_NAME_COUNT \
    ((int)(sizeof(s_gamma_preset_names) / sizeof(s_gamma_preset_names[0])))
#endif /* CONFIG_AMY_PCM_GAMMA808 */

const char *pcm_preset_name_for(uint16_t preset)
{
    if (preset < SEQ_PCM_NAME_COUNT) {
        return s_pcm_preset_names[preset];
    }
#if CONFIG_AMY_PCM_GAMMA808
    if (preset >= SEQ_GAMMA_NAME_FIRST &&
        preset < SEQ_GAMMA_NAME_FIRST + SEQ_GAMMA_NAME_COUNT) {
        return s_gamma_preset_names[preset - SEQ_GAMMA_NAME_FIRST];
    }
#endif
    return (const char *)0;
}

#endif /* CONFIG_SEQ_PATCH_SHOW_NAMES */
