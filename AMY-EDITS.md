# AMY Local Edits

Edits applied on top of the upstream `shorepine/amy` submodule.
Upstream commit: `0fb0a00` (v1.2.163, vendored 2026-09-05). The vendor base is
upstream `main` as is. Three of this repo's own upstream PRs merged after
this base; their code is still carried and is listed under "Merged upstream,
still carried until the next sync". The Karplus-Strong entries are an earlier
shape of the still-open #1204. Each active entry states its upstream status.
Previous bases: v1.2.160 `a89df0c`, v1.2.145 `55e044d`, v1.2.121 `85a7025`,
v1.2.104 `fd09bd2`, v1.2.31 `1e23c70`. The submodule tracks upstream `main`
(`.gitmodules` `branch = main`; refresh with `git submodule update --remote amy`).

Edits are marked `// LOCAL EDIT` in the source, except the Karplus-Strong rework
(`oscillators.c`, `amy.h`, `amy.c`, `filters.c`), which is documented by its
own comments (only the `render_ks` index loop and the `ram_caps_ks` field carry
a marker). ESP32-S3-specific edits are
permanent (upstream has no concept of IRAM/DRAM placement or FreeRTOS task
signatures); the fixes listed under "Dropped" were merged upstream and are no
longer carried here.

```mermaid
%%{init: {"theme": "base", "themeCSS": ".mindmap-node polygon { fill: #afb8c1 !important; }", "themeVariables": {
  "primaryColor": "#0969da", "primaryTextColor": "#ffffff", "lineColor": "#8c959f",
  "cScale0": "#d0d7de", "cScaleLabel0": "#1f2328",
  "cScale1": "#d0d7de", "cScaleLabel1": "#1f2328",
  "cScale2": "#d0d7de", "cScaleLabel2": "#1f2328",
  "cScale3": "#d0d7de", "cScaleLabel3": "#1f2328",
  "cScale4": "#d0d7de", "cScaleLabel4": "#1f2328",
  "cScale5": "#d0d7de", "cScaleLabel5": "#1f2328",
  "cScale6": "#d0d7de", "cScaleLabel6": "#1f2328",
  "cScale7": "#d0d7de", "cScaleLabel7": "#1f2328",
  "cScale8": "#d0d7de", "cScaleLabel8": "#1f2328"
}}}%%
mindmap
  root((components/amy<br/>local edits))
    c1{{"`**ESP32-S3 platform**`"}}
      48 kHz sample rate
      Fixed-point toggle
      ldexpf shifts
      Render lock
      PIE block clears
      IRAM attributes
      Saw LUT in DRAM
      IDF 6.0 task signatures
      Dual-core bus sum skipped
    c2{{"`**Memory and OOM**`"}}
      Delta-pool PSRAM spill
      sequencer_init guard
      ram_caps_sequencer
      Voice-list clamp
    c3{{"`**Render path**`"}}
      Oscillator arithmetic
      Unison cluster
      PCM retrig fade-restart
    c4{{"`**Karplus-Strong, PR 1204**`"}}
      Ring index and length
      Ring per osc
      Loop allpass
      Pluck position
      Gain ramp
      Excitation shaping, local
      Note-off release, local
    c5{{"`**Accessors and API**`"}}
      PCM frame and shrink
      gamma9001 map and size
      Voice base osc
      Patch oscs per voice
      Algorithm count
      Custom operator programs
    c6{{"`**Behaviour fixes**`"}}
      Periodic-entry horizon
      reset_osc keeps the bus
    c7{{"`**Build and diagnostics**`"}}
      Kconfig flags
      CMake settings
      COARSE profiler
```

## Dropped (merged upstream)

| PR | Description |
|----|-------------|
| [#740](https://github.com/shorepine/amy/pull/740) + [#743](https://github.com/shorepine/amy/pull/743) | `chained_osc` NULL guard in `render_osc_wave` (amy.c) |
| [#744](https://github.com/shorepine/amy/pull/744) | `init_stereo_reverb()` → `bool` return + OOM crash safety (delay.h, delay.c, amy.h, amy.c, api.c) |
| [#787](https://github.com/shorepine/amy/pull/787) (merged as [#809](https://github.com/shorepine/amy/pull/809), v1.2.25) | Reverb `LPF()` state passed by pointer - feedback crossover lowpass now actually filters (delay.c) |
| [#790](https://github.com/shorepine/amy/pull/790) (merged as [#811](https://github.com/shorepine/amy/pull/811), v1.2.26) | Reverb delay-line state hoisted into loop locals (delay.c) |
| [#764](https://github.com/shorepine/amy/pull/764) (`8ade0b1`, v1.2.13) | `MUL5A_SS` / `MUL6A_SS` float-mode fallbacks (amy_fixedpoint.h) |
| upstream [#826](https://github.com/shorepine/amy/pull/826) (`ba22f12`, for issue #791; v1.2.33, 2026-07-07) | `amy_grab_lock()` / `amy_release_lock()` prototypes in amy.h. The local edit also declared `amy_init_lock()` and an ESP `extern SemaphoreHandle_t amy_queue_lock`; neither has a caller outside amy.c (`amy_init_lock` is `amy_start`-internal), so the whole block retired 2026-09-05. |
| [#961](https://github.com/shorepine/amy/pull/961) (merged `ae469e1`, 2026-07-24) | OOM survival on the voice/event allocation paths: `amy_oom()` + `amy_get_oom_count()`, `bool ensure_osc_allocd()`, alloc-before-free breakpoint realloc (amy.c, amy.h, instrument.c, cv_trigger.c, interp_partials.c). Vendored tree realigned to the merged version 2026-07-25 - see that entry for what stays local. |
| [#993](https://github.com/shorepine/amy/pull/993) (merged `56c8f1d`, 2026-07-27) | `amy_oom()` logs only the first failure, counts the rest - the vfprintf ran on the render thread and OOM retries re-fail per note-on, flooding stderr from the audio path (amy.c). Vendored shape identical to upstream; found via the BLE-MIDI + additive-piano slowdown. |
| [#875](https://github.com/shorepine/amy/pull/875) + [#877](https://github.com/shorepine/amy/pull/877) | LUT trig (`sin2pi`/`cos2pi` over the quarter-sine table) in the biquad coefficient generators, `sin_lut`/`cos_lut` + `qsin_fxpt_lutable` (filters.c, log2_exp2.c, log2_exp2_fxpt_lutable.h). Was carried as a verbatim cherry-pick; retired on the v1.2.104 sync. |
| [#881](https://github.com/shorepine/amy/pull/881) (`470a6c0`, v1.2.55) | Float-suffixed literals in the biquad coefficient generators (filters.c). The local notch generator keeps its own suffixes (it postdates the fix and is still local). |
| [#951](https://github.com/shorepine/amy/pull/951) (v1.2.83) + upstream follow-ups | `SMUL64R` full-precision biquad multiply (`FILT_MUL_SS`) + BFP-free LPF24 path. Upstream evolved the vendored `_nobfp_fixedzeros` shape into `_once/_twice_fixedzeros` kernels with its own `AMY_HAS_MUL64`/`USE_BLOCK_FLOATING_POINT` split; the vendored tree now carries upstream's version verbatim. |
| [#949](https://github.com/shorepine/amy/pull/949) (v1.2.87) | `mod_osc_would_cause_loop()` cycle guard for chained modulators (amy.c) - our own PR, merged upstream. |
| [#827](https://github.com/shorepine/amy/pull/827) (v1.2.45) + [#982](https://github.com/shorepine/amy/pull/982) | Integer `amy_sysclock()` (our PR), then upstream's 64-bit `amy_sysclock64()` + wrap-relative `AMY_TIME_GEQ` + the 49.7-day rollover fix. #982 also adopted the µs-domain tick compare and the single-precision tempo math in sequencer.c, retiring the whole local api.c/sequencer.c clock family. |
| [#905](https://github.com/shorepine/amy/pull/905) + [#907](https://github.com/shorepine/amy/pull/907) (v1.2.64/65) | `AMY_IRAM_ATTR`/`AMY_DRAM_ATTR` macros (credited to this repo, now with a Tulip opt-out), the hot-path IRAM annotations for envelope.c, log2_exp2.c, delay.c, most of filters.c/oscillators.c/amy.c, and the clipping-LUT `AMY_DRAM_ATTR` placement. Only three annotations upstream lacks remain local (see below). |
| [#893](https://github.com/shorepine/amy/pull/893) (`a96d574`, v1.2.62) | FM scratch allocated as one flat 16-byte-aligned block via `malloc_caps_block` (algorithms.c) - supersedes the vendored per-pointer alignment; our three remaining `malloc_caps_block` call sites in amy.c stay local. |
| [#1000](https://github.com/shorepine/amy/pull/1000) (v1.2.106) | `FILTER_NOTCH` type - our own PR (half-angle center recovery, `dsps_biquad_gen_notch_f32`), merged upstream with dpwe test coverage in #1005 (amy.h, filters.c). |
| [#1020](https://github.com/shorepine/amy/pull/1020) | `FILTER_PHASER` type - our own PR (6-stage allpass chain, `allpass1_chain` + `dsps_phaser_f32_ansi`), merged 2026-08-01 (amy.h, filters.c). |
| upstream sequencer rework (#1017 lineage, 1.2.104->1.2.121) | `SEQ_LOCK` mutex + active-tag dense index (sequencer.c) - superseded wholesale. Upstream rewrote the sequencer: entries store raw wire strings, all link/string mutations run under `amy_queue_lock` (taken inside `sequencer_add_wire` and the tick fire path, released around `amy_play_message` because the parser can re-enter), and the tick walk is lock-free over index links threaded through a fixed array (single aligned-store splices, ascending order - a stale link can skip/revisit one tick, never walk freed memory). That closes the same use-after-free our SEQ_LOCK guarded and replaces the O(highest_tag) sweep with an active-only list, so both local edits retire. The int16 index-array shrink retires with them. Upstream has reshaped the table again since this base (#1156, see "Deferred / needs porting"). |
| upstream (1.2.104->1.2.121 interval) | UART MIDI poll guard in `amy_update_tasks()` (i2s.c) - upstream adopted the same `AMY_MIDI_IS_UART` gate with its own comment. |
| upstream (1.2.104->1.2.121 interval) | `AMY_RENDER_TASK_PRIORITY`/`AMY_FILL_BUFFER_TASK_PRIORITY` = `ESP_TASK_PRIO_MAX - 1` (amy.h) - upstream now carries the fix (Arduino gets `- 5`), retiring the 2026-03-22 edit. |
| [#1049](https://github.com/shorepine/amy/pull/1049) (v1.2.122) | Ingest/tick split: `flush_due_deltas()` settles due deltas without running the rendering-context-only sequencer tick service (amy.c) - our own PR, merged upstream. |
| [#1106](https://github.com/shorepine/amy/pull/1106) + [#1107](https://github.com/shorepine/amy/pull/1107) (v1.2.149) | Free a released voice's oscs (`RESET_FREE_OSC`, non-allocating describe paths) and the `ram_caps_oscs` config field for the per-osc state arena. Ours, vendored ahead of merge in the v1.2.145 base; `main/main.c` still points `ram_caps_oscs` at PSRAM. |
| [#1116](https://github.com/shorepine/amy/pull/1116) (v1.2.157) | Per-osc distortion stage: the `dist_block`/`dist_process` kernels and `dist_config_t`/`dist_state_t` split, the `G` wire sub-commands, distortion on the SILENT chained-osc head, the 10 Hz DC blocker on `DIST_CRUSH`'s wet path, the `SMULR6` pre-gain. |
| [#1134](https://github.com/shorepine/amy/pull/1134) (v1.2.160) | Distortion follow-ups: stage stacking (`dist_stages` bitmask), drive/mix on the control-coef rail, the per-bus stage. Merged in its POST-review shape - one set of `dist_*` event fields serves both scopes and the event decides which (an osc named means that osc, none means the bus the event addresses), so the separate `bus_dist_*` event fields, the `J` bus wire letter and `DIST_OFF` are gone. `fx_push_dist()` (amy_fx.c) authors the shared fields with no osc named. |
| [#1135](https://github.com/shorepine/amy/pull/1135) (v1.2.158) | `amy_update_handle` registered in `amy_platform_init()` gated on the audio config, fixing the multithread + non-I2S first-block deadlock. Contract: `amy_start()` must run on the task that calls `amy_update()`. Our config (`audio = AMY_AUDIO_IS_NONE`, `multithread = 0`) registers the init task's handle but nothing notifies it - benign. |
| [#1137](https://github.com/shorepine/amy/pull/1137) (v1.2.158) | Second-core render-done semaphore in `i2s.c`'s fill task; dormant here (`multicore = 0`). |

### Merged upstream, still carried until the next sync

The vendor base (v1.2.163) predates these merges, so the code is still a
local hunk in `components/amy/`. Upstream's version is the same code; the
hunk is dropped at the next sync.

| PR | Upstream | Local hunk |
|----|----------|------------|
| [#1159](https://github.com/shorepine/amy/pull/1159) | v1.2.171 | `chorus_max_delay` bounds the chorus sweep: `delay_line_in_out()`, `config_chorus()`, `CHORUS_DEFAULT_MAX_DELAY` 320 -> 512 (`delay.c`, `amy.c`, `amy.h`) |
| [#1163](https://github.com/shorepine/amy/pull/1163) | v1.2.169 | `play_delta` calls `reset_filter()` when a `FILTER_TYPE` delta changes the type (`amy.c`) |
| [#1171](https://github.com/shorepine/amy/pull/1171) | v1.2.175 | `render_lut_cub_sized` with six size-specialised cubic kernels, and `render_lut_fm_256` for the FM sine table (`oscillators.c`); upstream marks the kernels and the dispatcher `AMY_NOINLINE` and builds them only under `AMY_USE_FIXEDPOINT` |

## Active local edits

### `patches.c` - a synth-addressed `reset_osc` keeps the instrument's bus (upstream PR candidate)

`reset_osc()` puts the osc back on bus 0 (`reset_osc_params`), but the bus
is instrument state (`instrument_set_bus`). An event naming a synth and a
voice-relative `reset_osc` therefore moved that osc of every voice onto bus 0
while `instrument_get_bus()` still named the synth's bus, and later osc-level
config does not re-state it. `patches_event_has_voices()` now reads the
instrument's bus before it unsets `e->synth` and, when it is not 0, queues a
`BUS` delta for each voice's reset osc right behind the reset. Same rule as
upstream's re-patch bus inheritance in `patches_load_patch()`. In the app this
hit the clip players on every transport stop (a bounce clip left the clip bus
for bus 0) and a drum row on every PCM preset or mode change. A `BUS` delta
allocates its osc at play time, so a reset aimed at a never-allocated osc of
a bus > 0 synth now allocates it. Not posted upstream; upstream
`patches_event_has_voices()` (v1.2.188) still leaves the reset osc on bus 0.

### `amy.h` + `amy.c` + `oscillators.c` + `patches.c` + `api.c` — per-osc unison cluster (experimental, dev-only)

An exploratory engine-side unison, kept to settle by measurement whether
many-copy unison on this target is bound by per-osc bookkeeping or by LUT
arithmetic. Not upstream-shaped: no wire letter (struct events only, and the
readable event print carries the fields while the wire form drops them),
no test coverage, and the parameter model may change. The app-side layouts
it is measured against live in `components/synth_core` (`voice_unison_t`).

One osc renders `unison_count` detuned copies of its LUT wave into its own
buffer (`unison_prepare()` + a per-copy loop in `render_lpf_lut`,
`render_triangle`, `render_sine`, `render_wavetable`); envelope, filter,
distortion, pan and every mod rail stay one per osc. Copy `i` sits at
`logfreq + unison_offset + i * unison_spacing` (octaves) off the osc's
per-block `logfreq`, so NOTE, BEND, portamento and the pitch rails move the
whole cluster; `unison_blend` tapers the outer copies by position within the
cluster's own span and the weights are power-normalized. Copies 1..n-1 keep
their phase in `synthinfo.unison_phase[]` and respread from copy 0 at every
note-on (`unison_note_on()`). Deltas `UNISON_COUNT/SPACING/OFFSET/BLEND` sit
above `NOTE_SOURCE_CHANNEL` (the ids below are auto-numbered and never cross
the wire). `play_delta` clamps the count to 1..`AMY_UNISON_MAX` (8) and the
blend to 0..1. Count 1 (the default) is the previous single render, one step and
one amp pair per renderer. Waves without a LUT renderer (PCM, KS, ALGO,
partials, noise) ignore the count.

The terms that depend only on the cluster parameters - the copy ratios
`2^(offset + i * spacing)` and the power-normalized weights - live in a
one-entry cache (`unison_terms`, `unison_terms_fill()`) keyed on (count,
spacing, offset, blend) and refilled only when a parameter changes, so
`unison_prepare()` costs one float multiply and one `F2P` per copy per
block instead of an `exp2_lut` and a soft divide per copy plus a `sqrtf`.
Copy `i`'s frequency is the caller's `freq` times the ratio rather than
`freq_of_logfreq(logfreq + d)`; the two differ by the LUT-exp2
interpolation error only. One entry is correct because the render path is
single-threaded here (`multicore=0`); a multicore build would need one per
core.

### `oscillators.c` — render-path arithmetic (2026-09-18 asmdiff probe)

Codegen-driven edits from the per-function probe, each marked in place:

- `render_lpf_lut` renders the detuned unison copies (1..n-1, both pulse
  edges) through the linear kernel (`render_lut_sized`, below, which falls
  back to `render_lut`); copy 0 keeps the cubic kernel
  (`render_lut_cub_sized`), so a plain count-1 saw or pulse is bit-identical
  to upstream. Local only, with the unison cluster. The saw LUT set
  is chosen per period so the top harmonic stays under Nyquist, and a host
  A/B put the cubic-vs-linear difference signal at -49 dB and below
  (inaudible); `render_lut` is a 32-insn hardware loop, `render_lut_cub` a
  74-insn plain loop whose spilled counter no flag rescues. On target a
  cubic copy costs 20.6k cycles per block, a linear one 8.8k; going linear
  on copy 0 as well would save a further 11.6k per plain saw/pulse osc and
  was declined to keep plain voices unchanged.
- `render_ks` keeps the ring index in a register for the block and wraps it
  by compare (`if (next >= buflen) next = 0`) instead of the iterative
  hardware `rems` twice per sample; `phase` is written once after the loop.
  The peak tracker uses `|value|` for every sample; upstream's first-sample
  special case took the signed value, a difference only when sample 0 is
  negative and the reaper's threshold is at stake. With the tuning stage on
  and no dispersion stages (the default), `ks_render_tuned` runs instead:
  the block is split at the ring's wrap so the inner loop has no wrap test,
  and with pointer walks for the ring and the output it is a 29-insn
  hardware loop carrying the gain ramp (the index loop was 35). Its tuning
  allpass is the one-multiply form; the index loop's tuning stage uses the
  same form, so both read the allpass memory alike. Open upstream PR #1204
  renders KS with the same split-at-wrap loop and gain ramp, always tuned,
  and keeps the allpass memory in `synth[osc]->ks_tune_state`; the index
  loop is local.
- Constant divides folded to reciprocal multiplies (`freq * (1.0f / X)`
  with X a compile-time constant): `freq / AMY_SAMPLE_RATE` in
  `render_fm_sine`, `render_partial`, `unison_prepare`; `freq / mod_sr` in
  all five `compute_mod_*`. The S3 FPU has no divide, so each was a
  `__divsf3` libcall per block per osc; the product differs from the
  quotient by at most 1 ulp. Not posted upstream; upstream (v1.2.188)
  still divides.
- `render_wavetable`: `floor(interp)` -> `floorf(interp)`; the double
  promotion cost `__extendsfdf2` + `floor` + `__fixdfsi` per block per
  wavetable osc. Not posted upstream.

- `render_lut_sized`: size-specialised linear kernels for the detuned unison
  copies (2048..64-entry tables, `noinline` so each keeps its own loop;
  32 -> 28 instructions), byte-identical to the generic linear kernel in
  host-sim saw and pulse sweeps. Local only: it serves the unison cluster.
  Its cubic counterpart and the FM-256 kernel are upstream (#1171, see
  "Merged upstream, still carried until the next sync").


### `oscillators.c` — Karplus-Strong ring-index init + sample-rate-derived buffer length (open upstream PR #1204)

Two universal bugs, found 2026-08-25 while surveying the delay/comb machinery
and measured with a host sim of the vendored tree (fixed-point build).
Both are target-agnostic, so they belong upstream rather than here.

1. **`ks_note_on()` now zeroes `synth[osc]->phase`.** `render_ks()` uses
   `phase` as a plain integer ring index, but nothing initialised it and every
   other wave leaves a fixed-point phasor there (`P_FRAC_BITS = 31`). Measured:
   after 10 blocks of `SAW_DOWN`, `phase = 0x17fb6000`, so `(uint16_t)phase` =
   **24576** into an 802-entry row - one read *and one write* ~94 KB past the
   allocation per note-on, then back in range. It fired on every wave-to-KS
   switch on a reused osc, which is what preset scrolling does.

2. **`MAX_KS_BUFFER_LEN` derives from `AMY_SAMPLE_RATE`** (`AMY_SAMPLE_RATE /
   KS_LOWEST_FREQ + 1`) instead of the literal 802 = 44100/55, and `render_ks()`
   applies the same clamp `ks_note_on()` always had. The literal held one period
   of 55 Hz only at 44.1 kHz; at our 48 kHz the true floor was 59.85 Hz while
   the `freq >= 55` guard still admitted MIDI notes 33-34, overrunning the row
   by up to 70 samples every block for the life of the note. `KS_LOWEST_FREQ`
   also replaces the duplicated magic 55 in the render guard.
   Verified: at 48 kHz, `MAX_KS_BUFFER_LEN` = 873 vs a worst-case `buflen` of
   872. At 44.1 kHz the value is still exactly 802, so no behaviour changes there.

The larger shared-ring defect these two sat next to is fixed by the entry
below.

Both are the first two commits of open upstream PR
[#1204](https://github.com/shorepine/amy/pull/1204). The PR's version also
keeps `buflen >= 1` in `ks_note_on()` and skips notes at or above the sample
rate in `render_ks()`; the vendored code has neither guard.

### `oscillators.c` + `amy.h` + `amy.c` — per-osc Karplus-Strong ring binding (open upstream PR #1204, different shape)

`ks_buffer` is a pool of rings, but which ring an osc played was decided by a
single module-global cursor (`ks_polyphony_index`) that every KS osc
dereferenced at render time. Nothing recorded "this osc plays that ring", so
all simultaneous KS voices landed on the same one. Measured consequences
(host sim):

- voices sharing a ring damp each other every sample, so three KS notes were as
  loud as one (1.12x where independent voices give 1.73x) and decayed ~2.6x
  faster;
- `ks_note_on()` refilled the shared ring at full amplitude under every voice
  still reading it - a note-on at velocity 0.01, inaudible on its own, raised
  the voices already sounding by **+134%**;
- `ks_oscs > 1` was unusable: note-on filled ring N and left the cursor at N+1,
  which is the ring `render_ks()` then read, so a single note rendered silence.
  `ks_oscs = 1` worked only because the wrap sent N+1 back to 0.

**Fix.** `synth[].ks_index` (amy.h) records the ring, chosen in `ks_note_on()`
and read by `render_ks()`. Ownership is a two-way claim - `ks_row_owner[r] ==
osc` and `synth[osc]->ks_index == r` - so `reset_osc_state()` setting
`ks_index = KS_NO_ROW` (amy.c) releases the ring with no explicit free path.
`ks_alloc_row()` prefers the ring this osc already owns (a retrigger re-excites
its own string and disturbs nobody), then any idle ring, then steals the
quietest - the least audible collision. `ks_init()` also gained the OOM
handling AMY's other allocators have: all-or-nothing, `amy_oom()`, KS silent
rather than half-allocated. The ring pointer table, `ks_row_owner` and
`ks_ap_state` come from `ram_caps_synth` and the rings from `ram_caps_ks`
(below) instead of bare `malloc`; `ks_deinit()` is NULL-safe and frees all
of them.

After: 3 notes measure 1.48-1.65x (target 1.73x), a silent note-on moves the
ringing voices +9.5% instead of +134%, and `ks_oscs = 4` renders correctly.

`main/main.c` sizes `ks_oscs` from Kconfig (`SEQ_KS_RINGS`, or derived from
`SEQ_KS_LAYERS` x rows x `SEQ_KS_VOICES_MAX` plus arp and live voices when the
rings are in PSRAM); see the comment there.

`amy.h` + `api.c` + `oscillators.c`: `amy_config.ram_caps_ks` gives the rings
their own caps, defaulting to `ram_caps_synth` (`// LOCAL EDIT` in amy.h and
api.c). Only the rings move; the owner table and allpass state are a few bytes
per ring and stay in `ram_caps_synth`.

Upstream status: open PR [#1204](https://github.com/shorepine/amy/pull/1204)
fixes the same defect in a different shape. The ring is owned by the osc
(`synth[osc]->ks_ring`), allocated at its first KS note-on from
`ram_caps_oscs` and freed in `free_osc()`; there is no pool, owner table or
stealing, no `ks_init()`/`ks_deinit()`, and `ks_oscs` is only an on/off
switch. The vendored pool (`ks_index`, `ks_row_owner`, `ks_alloc_row()`,
`KS_NO_ROW`, `ram_caps_ks`) is an earlier design and goes away when the PR
is adopted, together with the `ks_oscs` sizing in `main/main.c`.

### `oscillators.c` + `amy.h` + `filters.c` - Karplus-Strong loop allpass: fractional period (open upstream PR #1204) and dispersion trial knobs (local)

`render_ks` dropped the fraction of `AMY_SAMPLE_RATE / freq`: `buflen = floor(P)`
and the two-tap average is centred half a sample ahead, so the loop closed at
`buflen - 0.5` samples and every KS note played sharp. Host sim
(KS minus a SINE control at the same MIDI note, 48 kHz):
+3.4 c at A2, +9.4 c at A4, +33.5 c at A5, +49.8 c at A6. A first-order allpass
after the average now completes the period, `a = (1 - D)/(1 + D)` with
`D = P + 0.5 - buflen` in [0.5, 1.5); `buflen` is unchanged. After: within
0.4 c at every measurable note. A7 decays below the harness floor inside the
first block (the average alone loses 2.7 % per pass there) and is unmeasured.
One float divide per block per KS osc for `a`.

Trial knobs, not for upstream: `ks_loop_set()` / `ks_loop_get()` (`amy.h`)
bypass the tuning stage (off plus 0 stages is the old loop plus the gain
ramp and `|value|` peak below, so bit for bit at constant gain) and
add up to `KS_DISPERSION_MAX_STAGES` stages of `allpass1_chain` for a string
stiffness B (0..`KS_STIFFNESS_MAX`, mode k at k f sqrt(1 + B k^2)). The
stage coefficient follows the note: `ks_loop_set()` fits one per semitone
(least squares on delay over modes 2..8 below about 4 kHz, golden-section
search) into a static scratch before taking the render lock, and
`ks_ring_len()`, which `ks_note_on` and `render_ks` share, interpolates it
and takes the chain's exact phase delay at the fundamental out of the ring.
Set from the app's DEV menu ("KS loop", Stages and Stiff); read once per
block. Host-measured, 3 stages: the fundamental stays within 0.1 c, and at
B = 1e-4 modes 2..6 land within 0.5 c of the stiff-string target from A2 to
A5, at B = 1e-3 within about 3 c. At 5e-3 a first-order stage can no longer
follow the curve and the low modes overshoot (mode 2 +21 c against +13 c).
Per block per KS osc with stages on: one `log2f`, `sinf`, `cosf` and two
`atan2f`.

State: `ks_ap_state`, one row per ring sized for the maximum stage count,
allocated in `ks_init()` from `ram_caps_synth` (all or nothing with the
rings, which come from `ram_caps_ks`), cleared in `ks_note_on`.
`allpass1_chain` and `FILT_MUL_SS` moved from `filters.c` to `amy.h`; the
phaser is unchanged.

Codegen (-O2 per TU): the index loop in `render_ks`, which runs with the
tuning stage off or dispersion stages on, is a plain 35-instruction loop
(upstream's was a plain 28-instruction span). The default path (tuning on,
0 stages) is `ks_render_tuned`, a hardware loop (render-path arithmetic
entry). The chain is unrolled so no nested loop sits inside it.

Upstream status: the tuning stage is commit 5 of open PR
[#1204](https://github.com/shorepine/amy/pull/1204), same math and
one-multiply form, always on and with its memory in
`synth[osc]->ks_tune_state` (one `SAMPLE` per osc, cleared at note-on). The
dispersion stages, `ks_loop_set()`/`ks_loop_get()`, `ks_ap_state` and the
move of `allpass1_chain`/`FILT_MUL_SS` to `amy.h` are local only.

### `oscillators.c` - Karplus-Strong pluck position from `duty` (open upstream PR #1204)

KS ignored `duty`; every note started from the same kind of burst, flat on
average and random per pluck (host sim, A2: harmonic 2 at +12 dB against
harmonic 1 on one pluck, -7 dB on the next). `ks_note_on` now reads
`duty_coefs[COEF_CONST]` as a pluck position `b = |duty - 0.5|`: the burst
becomes a mix of the noise, combed to notch the harmonics a pluck at b leaves
out, and a zero-mean pulse b of the period wide (an ideal pluck's bridge
force: the same notches on a 1/n slope, identical on every note). Pulse
share and comb depth ease in over b < `KS_PICK_RAMP` (0.1) to
`KS_PICK_MIX` (0.6), so the first step off 0.5 stays near the plain burst
(A2, duty 0.49: within 3.1 dB of it at harmonics 1-16).

Default unchanged: the osc reset leaves duty at 0.5, b = 0, and the old
mean-removal loop runs; the burst is bit-exact, and the fill consumes the
random stream exactly as before. `duty` and `1 - duty` give the same pluck.
`M < buflen` keeps a one-sample ring (freq above half the sample rate) at
duty 0 or 1 off `ks_pluck`, where the pulse duty would be 1 and its scale
divides by zero.

Cost: note-on only, `render_ks` untouched. `ks_pluck()` folds the mean
removal, comb, rescale and pulse into the one pass that already removed the
mean (two passes over the ring, as before), fixed point per sample
(`FILT_MUL_SS` twice); scales come from the fill's expected RMS, three
`sqrtf` and a few divides per note. Against a float prototype that measured
RMS per note: whole-file difference -29 to -43 dB, per-note level within
1 dB (the short A5 ring varies most). Reads the constant coefficient on
purpose: the pluck is a patch setting, and `msynth->duty` would also be
stale at note-on (it is refreshed only for sounding oscs, so a duty change
made while the voice is silent would reach the note after next).

Upstream status: commit 7 of open PR
[#1204](https://github.com/shorepine/amy/pull/1204) carries the same
`ks_pluck()`, `KS_PICK_MIX` and `KS_PICK_RAMP`; the PR multiplies with
`SMULR7` where the vendored copy uses `FILT_MUL_SS`.

### `oscillators.c` + `amy.h` - Karplus-Strong excitation shaping (local trial)

`ks_note_on` fills the ring with uniform noise and removes its mean; the
burst's spectrum is random per note (host sim, A2: harmonic 2 at +12 dB
against harmonic 1 on one pluck, -7 dB on the next) and does not follow
velocity. Trial knobs `ks_excite_set()` / `ks_excite_get()` (`amy.h`) shape
the burst once per note-on in `ks_shape_burst()`: a circular comb
`x[i] - g x[i - M]` at the pick position, depth `g` 0..1 (in place, walking
the gcd cycles of `i -> i - M`; a short M at full depth is close to a
differentiator, so a shallow depth keeps a pick near 0 close to the unshaped
burst), a mix toward a zero-mean pulse `M` samples wide (an ideal
pluck's bridge force: the same notches on a 1/n slope, identical on every
note), and a circular one-pole lowpass whose cutoff, in harmonics of the
note, runs geometrically from `soft` at velocity 0 to `hard` at velocity 1.
The result is rescaled to the unshaped burst's RMS. All zero skips the call,
so the default is the old note-on bit for bit. Measured against theory on
A2: comb at 0.5 puts the even harmonics 25-38 dB down; the pulse at 0.2
matches `sin(n pi b)/n` within 0.3 dB through harmonic 8.

Cost: float, note-on only; about four passes over `buflen` (873 samples
worst case) plus one `powf` and one `expf`. Nothing in `render_ks`.
Written under the render lock, read by `ks_note_on`. Local only; not part
of PR #1204, whose `duty` pluck covers the comb and pulse but not the
velocity lowpass.

### `oscillators.c` - Karplus-Strong gain ramp (open upstream PR #1204)

`render_ks` multiplied the whole block by `msynth->amp`, which
`hold_and_modify()` has already advanced to the envelope's value at the end
of the block, and never read or wrote `last_amp`. An amp envelope on a KS
osc therefore moved in one step per block (187.5 Hz at 48 kHz, 256-sample
blocks): a decay zippered and a note-on played its first block at the
end-of-block level. The gain now ramps from `last_amp` to `amp` across the
block with the `incremental_amp` idiom of `render_envelope()`, and
`last_amp` advances at the end. A constant gain is bit-exact with before.
Cost: one add per sample. Commit 6 of open PR
[#1204](https://github.com/shorepine/amy/pull/1204) carries the same ramp.

### `oscillators.c` + `amy.h` + `amy.c` - Karplus-Strong note-off release (local trial)

`play_delta`'s note-off sets `note_off_clock` only in its default case, and
KS has its own case, so a KS note-off never started the amp envelope's
release: the gate and the release time did nothing, the string rang on at
the sustain level, and since `OSC_IN_RELEASE()` gates the silence check, a
KS osc with a sustain above zero rendered its loop every block until reset.
Upstream excludes KS on purpose (an osc with no envelope of its own would go
silent at note-off under the default `bp0`). `ks_note_off()` now returns
whether to release, and the KS case then starts the release as the default
case does. Trial knob `ks_release_set()` / `ks_release_get()` (`amy.h`),
default on; off is the upstream behaviour bit for bit. Set from the app's
DEV menu ("KS loop > Release"); written under the lock.

Upstream status: commit 3 of open PR
[#1204](https://github.com/shorepine/amy/pull/1204) takes a different rule.
It deletes `ks_note_off()` and sends KS through the default release unless
EG0 is still the key gate `reset_osc_params()` installs
(`eg0_is_default_gate()`), so a KS osc on the default envelope rings out at
note-off there and releases here. `ks_release_set()`/`ks_release_get()` are
local only.

### `algorithms.c` + `amy.h` — `amy_num_algorithms` count export (upstream PR candidate)

`const uint16_t amy_num_algorithms`, derived from `sizeof(algorithms)/sizeof(algorithms[0])`
near the end of `algorithms.c` (after `amy_block_zero_blocks()`), with an
`extern` in `amy.h`. API users stepping or validating `amy_event.algorithm` need
the real table size: upstream's `render_algo` indexes `algorithms[]` unchecked,
so any out-of-range value is an OOB read there (the vendored `render_algo`
clamps through `algorithm_for()`, next entry), and hardcoding 33 breaks the
moment the table grows. App consumers: the sequencer's Shift+Turn algorithm
stepper and the FM screen's ALGO row wrap. Not posted upstream. (The comments
on the definition and on the `amy.h` extern still say `render_algo` is
unchecked; that is upstream's behaviour, not the vendored one.)

**Rollback:** drop the definition in `algorithms.c` and the `extern` in `amy.h`;
consumers then need a local count define, and `amy_algorithm_ops()` (next
entry) reads the count too.

### `algorithms.c` + `amy.h` — custom operator programs + `amy_algorithm_ops()` read accessor

Two RAM rows (`custom_algorithms[AMY_NUM_CUSTOM_ALGORITHMS]`, `AMY_NUM_CUSTOM_ALGORITHMS = 2`)
sit behind algorithm indices `amy_num_algorithms + slot`; `render_algo` resolves its
row through `algorithm_for()` (fixed table below the count, custom rows above, clamped
to the last row instead of an OOB read). `amy_set_custom_algorithm(slot, ops)` writes a
row with six plain byte stores and no lock: the app double-buffers across the two rows
and only rewrites the row no live osc references, then switches `algorithm` +
`algo_source` in one FIFO event under `amy_queue_lock`. `amy_algorithm_ops(index)`
returns any program's six routing bytes (NULL past both ranges) so the operator-graph
editor can draw the table row a voice is playing. The `FmOperatorFlags` bit values are
documented next to the prototypes in `amy.h` (the enum stays file-private upstream).
App consumers: `custompatches/fm_voice.c` (publish/read), `custompatches/fm_graph.c`
(compiler, mirrors the bit values). Local only.

**Rollback:** drop the two functions + `custom_algorithms`/`algorithm_for` in
`algorithms.c`, restore `algorithms[synth[osc]->algorithm]` in `render_algo`, drop the
prototypes/define in `amy.h`; the FM custom-topology mode then has nowhere to render.

### `pcm.c` — retrig fade-restart (gated; replaces the zero-cross defer by default)

Upstream's retrig-into-active-PCM path (#1070) defers the new onset to the
next zero crossing of the old tail - a VARIABLE 0..512-frame latency that
depends on the tail's phase at the retrig instant. On a steady bass-drum
pattern whose sample outlasts the step spacing, every hit retrigs mid-tail
and the onset lands with per-hit-varying delay: host-measured 27 ms of
onset wander across 16 hits at 560 ms spacing (gamma9001 909 BD, note 39),
audible as an inconsistent kick transient. Deep pitches are the worst case
twice over: longer tails keep the osc active, and the LF cycle exceeds the
512-frame search window so the fallback splices at the window's quietest
sample instead of a true zero.

The edit replaces the defer with a **fade-restart**: play `PCM_RETRIG_FADE_FRAMES`
(64) more frames of the old tail under a linear ramp to zero (applied in
`render_pcm`), then splice to the new note - same `PCM_LOOP_ONCE_INTERNAL`
machinery, same click-free splice, but CONSTANT latency. Host A/B: onset
spread 27.4 ms -> 5.4 ms (= pure block quantization, identical to an
idle-start control); no click-energy regression.

Placement: the gated block sits inside upstream's `want_stretch` /
`fresh_start` restart branch of `pcm_note_on()`, so a `fit=` note never takes
it - the fit engine renders through `render_pcm_stretch()`, whose grains are
windowed and click-free by construction, and only the non-stretch branch arms
`PCM_LOOP_ONCE_INTERNAL`. The `render_pcm()` gain ramp is likewise unreachable
from a stretched note.

Gate: `AMY_PCM_RETRIG_ZERO_CROSS` (pcm.c) - define to 1 to restore the
upstream defer verbatim (host-verified to reproduce the pre-edit behavior
exactly). Not posted upstream; upstream `pcm_note_on()` (v1.2.188) still
defers to the zero crossing. The edit also rewords the comment at the top of
the restart branch and drops a stray `;;` on the `loopstart` line.

**Rollback:** build with `-DAMY_PCM_RETRIG_ZERO_CROSS=1`, or drop the three
`LOCAL EDIT` blocks in `pcm.c` (gate defines, `pcm_note_on` retrig branch,
`render_pcm` gain ramp).

### `algorithms.c` / `amy.c` — PIE block clears in `amy_render()` (local extension of upstream's kernel)

Local: `amy_render()`'s block clears go through upstream's PIE `zero()`
kernel, which upstream itself uses only for the FM scratch. The kernel is
upstream's own ([#893](https://github.com/shorepine/amy/pull/893): inline
`zero()`/`copy()` in `algorithms.c`, aligned FM scratch via
`malloc_caps_block`) and is not a local edit. The old local shape -
`components/pie_dsp` + `src/amy_simd.h` + `AMY_BLOCK_BZERO`/`BCOPY` - was
deleted 2026-07-25 (`7c498f6`, in git history if needed).

| Where | What | Why |
|-------|------|-----|
| `algorithms.c`, after `render_algo` | `amy_block_zero_blocks(SAMPLE *p, int nblocks)` - loops upstream's `zero()` | reach the kernel from `amy.c` without a second copy of the asm |
| `amy.h`, after `malloc_caps_block` | its prototype | - |
| `amy.c` `amy_render()` x3 (`fbl`, `per_osc_fb`, chorus `delay_mod`) | `bzero(...)` -> `amy_block_zero_blocks(p, 1)`; `fbl` passes `AMY_NCHANS` | `zero()` hardcodes one block = `AMY_BLOCK_SIZE * sizeof(SAMPLE)` = exactly `per_osc_fb` / `delay_mod`; `fbl` is `AMY_NCHANS` of them, so no length parameter is needed |
| `amy.c` `oscs_init` x2, `alloc_chorus_delay_lines` x1 | `malloc_caps` -> `malloc_caps_block` | `zero()` falls back to libc on an unaligned base, so without this the acceleration silently does nothing. Allocator body/gate are upstream's; upstream itself now aligns the FM scratch (#893), leaving these three call sites as the local delta |

Not measured: the 10.4% dx7 6-op figure is the FM scratch alone. Verified in the
ELF that the wrapper inlines away and `amy_render` carries three `loopnez` +
`ee.vst.128.ip` loops - **recheck after a re-vendor**: if it stops inlining it
becomes a flash call from IRAM `amy_render`, fix by marking it `AMY_IRAM_ATTR`.
(Re-checked on the v1.2.104 sync build, 2026-07-28: still 3x `loopnez` +
3x `ee.vst.128.ip`, no standalone symbol.)

Scope is deliberately narrow, and that is the finding rather than a shortcut:
PIE multiplies only on 8/16-bit lanes (`EE.VMULAS.S16` -> 40-bit QACC/ACCX),
while `SAMPLE` is s8.23 = int32, so every hot-path multiply (`MUL8_SS`,
`SMULR6`, `top16SMUL`) is a 32x32 product PIE cannot vectorize. On top of that
the LUT oscillators are gather-indexed (wavetable indexed by phase accumulator;
PIE has no gather) and the biquads/EQ/echo/chorus/reverb are all recurrence-
bound. esp-dsp independently reaches the same conclusion: its own ESP32-S3
biquad (`dsps_biquad_f32_aes3.S`) contains zero PIE instructions and is
hand-scheduled scalar FPU.

**`filters.c` is deliberately *not* routed through PIE.** `scan_max()` and
`block_norm()` are multiply-free reductions, so they looked eligible and an
earlier revision vectorized them. On-target A/B said no: that bought nothing on
any scene (four were flat to 0.00%) and cost up to 0.9% on filter-heavy ones.
Nearly every call is on a tiny buffer — `scan_max(w, 4)`, `scan_max(w, 6)` for
LPF24, and `scan_max`/`block_norm` over the 8-entry `filter_delay` — where the
vector setup costs more than the scalar loop it replaces, and those buffers sit
inside `synthinfo` so they are unaligned as well. Multiply-free is necessary but
not sufficient: the operation also has to be *long* enough to amortise the setup,
and in AMY only the bulk block clears and copies are. Don't re-add it.

Re-vendor note: done on the v1.2.104 sync - `zero()`/`copy()`,
`malloc_caps_block`, and the aligned FM scratch all arrived with upstream;
only the rows above remain ours.

### `src/amy.c` — skip the dead dual-core bus sum

`AMY_DUALCORE` is defined unconditionally for `ESP_PLATFORM`, but this build
runs `multicore = 0`, so `amy_render()` is only ever called with `core = 0` and
`fbl[1]` stays at its alloc-time zero fill forever. The mix-down loop was
therefore summing 512 int32 zeros per bus, every block, for nothing. Now
guarded on `amy_global.config.platform.multicore` — a runtime test, not
compile-time, so the sum reappears correctly if multicore is ever enabled.
Local only; upstream (v1.2.188) still sums unconditionally under
`AMY_DUALCORE`.

### `src/amy.h` — 48 kHz sample rate on ESP

`ESP_PLATFORM` joins the `__EMSCRIPTEN__` case of the sample-rate block
(`#elif defined __EMSCRIPTEN__ || ESP_PLATFORM`), so ESP builds render at
48000 (upstream's generic fallback is 44100). Must match
`CONFIG_UAC_SAMPLE_RATE`; a mismatch detunes/distorts USB audio. Any
re-vendor silently reverts this - after every AMY update, verify
`grep -n '__EMSCRIPTEN__ || ESP_PLATFORM' src/amy.h` finds the line. Local
only.

### `src/amy.h` — Kconfig-gated fixed-point toggle

`#define AMY_USE_FIXEDPOINT` replaced with a `#ifdef CONFIG_AMY_USE_FIXEDPOINT`
guard. Enabled via menuconfig: **AMY Synthesizer → Use fixed-point arithmetic**
(Kconfig default `y`; **=y in the current sdkconfig**).
Requires `components/amy/Kconfig` (new file, not upstreamed). Local only;
upstream still defines `AMY_USE_FIXEDPOINT` unconditionally.

### `src/amy_fixedpoint.h` — `ldexpf` SHIFTL/SHIFTR

One edit to the `#ifndef AMY_USE_FIXEDPOINT` (float mode) section (the former
`MUL5A_SS`/`MUL6A_SS` fallback edit merged upstream in #764):

**`SHIFTL` / `SHIFTR` use `ldexpf` instead of `exp2f`** — the original float
macros were `(s) * exp2f(b)`. When `b` is a runtime variable (e.g.
`exp2_lut()` integer part), `exp2f(runtime_int)` cannot be constant-folded
and emits a transcendental libcall. `ldexpf(s, b)` is the correct primitive
for ×2^n scaling and is never worse. **Caveat (verified by objdump):** on
this Xtensa LX7 toolchain GCC does *not* lower `ldexpf` (or `floorf`) to the
hardware `FLOOR.S`/exponent ops — both remain `call8` libcalls. So this is a
correctness/clarity win and a marginal speedup at most, NOT the fix for
float-mode CPU cost. Float mode is dominated by per-sample `floorf` libcalls
in `INT_OF_S` / `S_FRAC_OF_S` / `P_WRAPPED_SUM`; fixed-point
remains the product mode on this target. The same section also re-aligns the
`SMULR6`/`SMULR7` defines (whitespace only). Local only; upstream's float
macros still use `exp2f`.

### `src/amy.c` — Render lock

`amy_render()` wrapped with `amy_grab_lock()` / `amy_release_lock()` spanning
the entire function body (the `AMY_IRAM_ATTR` on it is upstream's own).

**Why:** `synth[]` / `msynth[]` arrays are structurally mutated by
`free_osc` / `alloc_osc` / `reset_osc` / patch loads on Core 0, while the
render walks the same pointers on Core 1. Without the lock a patch toggle can
free `synth[osc]` between the NULL check and the deref in `hold_and_modify`,
producing a `LoadProhibited` fault (EXCVADDR=0x8). App code that takes
`amy_grab_lock()` to read or change AMY state against the render
(`custompatches/drum_cache.c`, `wavetable_bank.c`, `clip_player.c`,
`sample_rec.c`) relies on this hold.

Local only. Upstream has since added its own render lock (#1205, v1.2.188),
a separate recursive lock taken before the queue lock, and stopped allocating
oscs on the ingest path (#1190, v1.2.181); upstream's `amy_render()` still
takes no lock. See "Deferred / needs porting".

### `src/amy.c` — delta-pool PSRAM spill, no-abort cap

`deltas_pool_alloc()` takes an explicit `caps` argument (and is `static`);
`deltas_add_pool_block()` allocates block 0 from `ram_caps_synth` and every
overflow block from `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`. Hitting
`MAX_DELTA_BLOCKS` reports through `amy_oom()` and returns instead of
upstream's `abort()`; a NULL block likewise returns with the pool untouched
(`delta_get` guards the empty pool).

**Why:** block 0 is the hot common-case path walked every audio block and
stays internal; growth beyond it happens only under event bursts, where
PSRAM latency is irrelevant and internal heap is the scarce resource. On
target a dropped event beats taking the synth down. Markers are dated
2026-06-27; the "no-abort cap" half is target policy, the PSRAM spill is
ESP-specific - neither is an upstream candidate.

**Rollback:** restore upstream's two-argument `deltas_pool_alloc` and the
`abort()`; every caller already tolerates a NULL return.

### `src/filters.c` + `src/oscillators.c` — residual IRAM annotations

Upstream adopted the hot-path `AMY_IRAM_ATTR` annotations wholesale (#905/#907)
except for three functions it left unannotated: `dsps_biquad_f32_ansi`,
`dsps_biquad_f32_ansi_split_fb` (filters.c) and `render_lut_fm_fb`
(oscillators.c). All three sit in the per-block render loop, so the local
annotations stay until upstream adds its own.
### `src/saw_lutset_fxpt.h` — saw LUT set pinned to internal DRAM

All 15 `saw_fxpt_lutable_N` tables carry `AMY_DRAM_ATTR`, the placement upstream
already gives the sine table. Saw and pulse render through `render_lut_cub`,
four table reads per sample (the detuned unison copies through `render_lut`,
two); from flash the tables are served through the PSRAM data
cache, and the 2026-09-18 feature-cost bench put a saw copy at 3.4x a sine copy.
Costs ~16 KB of internal DRAM (the four sizes melodic notes reach, 2048 to 256
entries, are ~7 KB of that). Rollback: drop the prefix; the header is otherwise
identical to upstream. Local only.
### `src/i2s.c` — IDF 6.0 task signature

`esp_fill_audio_buffer_task()` → `esp_fill_audio_buffer_task(void *pvParameters)`.
IDF 6.0 FreeRTOS requires `TaskFunction_t` (`void (*)(void*)`); the old
no-parameter form causes a type mismatch warning/error on `xTaskCreatePinnedToCore`.
(The former companion edit here, the `AMY_MIDI_IS_UART` poll guard, was adopted
upstream in the 1.2.104->1.2.121 interval - see the Dropped table.)

### `src/amy_midi.c` — IDF 6.0 task signature

`run_midi_task()` → `run_midi_task(void *pvParameters)`. Same FreeRTOS
`TaskFunction_t` fix as `i2s.c` above. Local only, like the `i2s.c` one:
upstream's task functions (v1.2.188) still take no parameter.

### `src/sequencer.c` — `sequencer_init()` OOM guard

Upstream's rewritten `sequencer_init()` (wire-string sequencer, v1.2.121)
writes the whole table through an unchecked `malloc_caps()` result. On failure
the guard now reports via `amy_oom()`, sets `max_sequences = 0` /
`first_active = -1`, and returns - `sequencer_add_wire()` and
`sequencer_check_and_fill()` already treat `sequences == NULL` as "not
initialized", so the sequencer disables wholesale instead of crashing at boot.
Same OOM-policy family as the #961 guards. Not posted upstream; upstream
`sequencer_init()` (v1.2.188) still writes through the unchecked result.

(The former SEQ_LOCK mutex and active-tag dense index were superseded
wholesale by upstream's sequencer rework - see the Dropped table for the
thread-safety argument of the new design.)

### `src/sequencer.c` + `src/sequencer.h` — periodic-entry horizon (upstream PR candidate)

`sequencer_set_periodic_horizon(tick)` / `sequencer_clear_periodic_horizon()`:
while set, repeating entries do not fire at or after the tick; one-shots are
unaffected. The tick walk tests it after the period match: one flag read
per periodic hit, plus a signed tick compare while it is set. The loop bounce sets it at its end tick so every looping note-on (the
sequencer tracks' periodic tags and the arp's) stops on the bar line exactly,
wherever the UI task is; the commit clears it after the mute flags are set.
Generic enough for any host that freezes a loop on a beat.

C-only setters, no wire form. Not posted upstream. Upstream's #1156
(v1.2.166) has since reshaped the table this hooks into; see "Deferred /
needs porting".

### `pcm.c` + `amy.h` — `pcm_osc_frame()` read accessor (upstream PR candidate)

A PCM osc's source position in frames: the stretcher's input timeline while
it is engaged, else the plain playhead (`INT_OF_P(phase, PCM_INDEX_BITS)`,
which only `pcm.c` can compute since the index split is private to it). The
loop bounce's drift guard compares it with the frame the tick grid implies on
every bar line. Read only, guarded for out-of-range and unallocated oscs;
same shape as the `amy_voice_base_osc()` accessor.

Not posted upstream.

### `pcm.c` + `amy.h` — `pcm_shrink_preset()` (upstream PR candidate)

Trims a memory preset to its final frame count once known: the loop bounce
records open-ended against a reserved maximum and cuts the block down at the
stop press. One `realloc` of the `[list node | preset | samples]` block; a
shrink is an in-place trim in the ESP heap (TLSF) and in libc, so no copy and
no move, but the list link and internal pointers are re-pointed if an
allocator ever did move it. Under the AMY lock like `pcm_load()`; loopend
capped to the new length, and a loopstart at or past it reset to 0. The
function branches on `ESP_PLATFORM` for `heap_caps_realloc` (with
`ram_caps_sample`), plain `realloc` elsewhere. Not posted upstream.

### `src/amy.h` + `src/amy.c` — COARSE profiler mode

Upstream gates the whole profiler behind `AMY_DEBUG`, which times *every* tag —
including per-osc/inner tags that fire dozens of times per block **inside** the
`AMY_RENDER` window. Those nested timestamp calls inflate the `AMY_RENDER` total,
which would overstate the parallelizable fraction in a dual-core feasibility
measurement.

Added a second, lighter profiling level, `AMY_PROFILE_COARSE` (Kconfig:
`AMY_PROFILE_MODE`, see `components/amy/Kconfig` + `CMakeLists.txt`):

- The profiler tables / timers / `amy_profiles_init/print` now compile under
  `#if defined(AMY_DEBUG) || defined(AMY_PROFILE_COARSE)` (was `#ifdef AMY_DEBUG`).
- In coarse mode `AMY_PROFILE_START/STOP` act only on the outer stage tags
  (`AMY_RENDER`, `AMY_FILL_BUFFER`, `AMY_EXECUTE_DELTAS`, `AMY_ESP_FILL_BUFFER`)
  via `AMY_TAG_IS_COARSE(tag)`. Because `tag` is a compile-time enum literal at
  every call site, the guard folds to a constant and inner call sites compile to
  nothing — zero overhead and no inflation of `AMY_RENDER`.
- Macros expand to a complete statement with no trailing `;`, matching upstream
  call sites that omit the semicolon (e.g. `AMY_PROFILE_START(AMY_RENDER)`).

Full `AMY_DEBUG` behaviour is unchanged (select `AMY_PROFILE_FULL`).

**Cross-core reset fix (`AMY_PROFILE_INIT`):** the dump runs on Core 0 (the
`app_main` idle loop) while render `START/STOP` run on Core 1. The upstream reset
zeroed `profiles[tag].start`; when that landed between a Core-1 `START` and
`STOP`, the `STOP` computed `(now - 0)` ≈ uptime, producing one ~uptime-µs spike
per window on whichever tag was mid-flight (observed as exactly one tag per
window reading billions of µs). Fix: `AMY_PROFILE_INIT` no
longer zeroes `.start` (it is only ever read by a `STOP` that follows a `START`,
so it never needs zeroing). `us_total`/`calls` are still reset each window; the
remaining `+=`-vs-`=0` race can at most carry one window's totals into the next
(both `us_total` and `calls` scale together, so **`us per call` stays correct**;
only that window's `% wall` may read high). Benefits coarse and full modes.

Kconfig also carries `AMY_PROFILE_INTERVAL_MS` (dump interval, default
5000), read by the app (`components/diagnostics/diag_report.c`), not by AMY.
Local only; upstream's `AMY_PROFILE_INIT` (v1.2.188) still zeroes `.start`,
and that fix is not posted.

### `Kconfig` + `CMakeLists.txt` — wavetable oscillator build flag

Upstream's `wave=WAVETABLE` oscillator (`oscillators.c`, `pcm_tiny.h`,
`pcm_samples_tiny.h`) was already fully implemented in the vendored source but
gated behind a bare, unwired `#ifdef AMY_WAVETABLE` — no build path ever
defined it, so the feature was silently dead code. This edit touches no source
inside `components/amy/src/`; it only wires the existing gate to a
Kconfig option (`AMY_WAVETABLE`, default **y**), mirroring the
`AMY_USE_FIXEDPOINT` pattern above:

```
if(CONFIG_AMY_WAVETABLE)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC AMY_WAVETABLE)
endif()
```

Measured cost (2026-07, this target): **+163,952 bytes flash `.rodata`** (5
built-in 64-cycle tables × 16384 samples × 2 bytes), **zero DIRAM/IRAM/PSRAM**
— `pcm_get_sample_ram_for_preset()` returns a pointer straight into the flash
`pcm[]` array, never RAM-copied. Verified via `idf.py size`
before/after on an otherwise-identical build. Local only (upstream has no
ESP-IDF `Kconfig`/`CMakeLists.txt`).

### `Kconfig` + `CMakeLists.txt` — Gamma TR-808 PCM bank flag

Same pattern as `AMY_WAVETABLE` above — no vendored source is modified.
Kconfig option `AMY_PCM_GAMMA808` (default **y**) defines upstream's own
`GAMMA9001` compile-time switch (introduced in v1.2.31's Gamma9001 work),
which selects:

- `amy.c`: ROM PCM bank = `pcm_gamma808.h` (19 full-length TR-808 samples)
  instead of the legacy 11-sample `pcm_tiny.h`;
- `patches.h`: the patch-258 "MIDI drums" string matching that bank's map;
- `pcm.c`/`amy.h`: the gamma9001 streaming hooks (`amy_set_gamma9001_pcm()`
  + presets at `GAMMA9001_PRESET_BASE`+, NULL-guarded and inert until a blob
  is provided, e.g. mmapped from a flash partition).

PCM preset numbering differs between banks; the sequencer drum defaults in
`components/synth_core/sequencer_core/seq_core_synth.c` follow
`CONFIG_AMY_PCM_GAMMA808`. Wavetable presets are unaffected (addressed via
`pcm_wavetable_base`). Cost ≈ +268 KB flash `.rodata` (XIP-cached, never
RAM-copied); zero DRAM/PSRAM/IRAM.

The same CMake block adds a `drums-flash` target
(`esptool_py_flash_target` + `esptool_py_flash_to_partition`) that writes
`components/amy/drums.bin`, the gamma9001 blob, to the `drums` data
partition when the file exists. It is not part of the default flash
target. Local only.

### `pcm.c` + `amy.h` — gamma9001 map read accessors (upstream PR candidate)

`amy_gamma9001_preset_span(preset, &span)` returns a map entry (blob offset,
length, loop points, midinote, sample rate); `_preset_base()` / `_preset_count()`
give the range. The map lives only in `pcm_gamma9001.h`, which defines the
array and so cannot be included twice. With these a platform that cannot
afford the 3.6 MB blob in RAM or mapped (PSRAM XIP leaves the S3 MMU no
57-page run) keeps the blob in flash and loads single presets with
`pcm_load()` under their gamma numbers, which shadow the blob entries in
`get_preset_for_preset_number()` - upstream's own mechanism, no lookup
change. Consumer: `components/synth_core/custompatches/drum_cache.c`
(windows the drum layers' presets into PSRAM on demand, unloads deferred
past the osc reset). Not posted upstream.

### `pcm.c` + `amy.h` — `amy_gamma9001_pcm_bytes()` accessor (LOCAL EDIT)

Two-line helper returning `GAMMA9001_BIN_FRAMES * 2`. The constant lives only
in `pcm_gamma9001.h`, which also defines the map array and so cannot be
included a second time; the ESP32-S3 mount code (`main.c
gamma9001_pcm_mount()`) checks the blob against the partition size with it.
(It used to size a flash mmap or a PSRAM fallback copy; since 2026-09-08 the
blob stays in flash and `drum_cache.c` loads presets singly, see the map
accessors above.) Not posted upstream (platform-neutral). The first of
the two `LOCAL EDIT` comments above it in `pcm.c` still describes the old
mmap / PSRAM-copy use.

### `amy.h` + `api.c` + `parse.c` + `sequencer.c` - `ram_caps_sequencer` for sequencer wire strings

`amy_config_t.ram_caps_sequencer` (default `ram_caps_events`, set in
`amy_default_config()`) gives the sequencer's wire strings their own caps at
the three sites that allocate them: the serialize buffer in `amy_add_event()`'s
ticks path (api.c), the stripped payload in `handle_ticks_message()`
(parse.c) and the per-fire copy in `sequencer_process_tick()` (sequencer.c).
Each site falls back to `ram_caps_events` before it reports through
`amy_oom()`. `main/main.c` points the field at PSRAM and keeps
`ram_caps_events` internal: the strings are control-path data parsed once
per fire, and with them out of the internal pool, an exhausted internal heap
no longer drops scheduled events and tag cancels. Not posted upstream;
upstream (v1.2.188) still allocates all three from `ram_caps_events`.

### `patches.c` + `amy.h` - `amy_voice_base_osc()` read accessor

`bool amy_voice_base_osc(uint16_t voice, uint16_t *base_osc)` returns a
voice's base oscillator from `voice_to_base_osc`, a global with no header
declaration, or false when the table is not initialised, the voice is out of
range or its entry is unset. Read only. Consumers: the filter editor's scope
(`synth_ui/ui_editors.c`), which maps an edit target's synth to the
oscillators carrying its filter with `instrument_get_num_voices()` for the
voice list, and `custompatches/clip_player.c`. Not posted upstream.

### `patches.c` + `amy.h` - `amy_patch_oscs_per_voice()` read accessor

Returns a patch's oscs per voice (`patch_oscs[]` for built-in patches,
`memory_patch_oscs[]` for user patches, 0 for undefined or reserved numbers),
so an embedder can budget `num_voices` before a load instead of finding the
osc pool exhausted partway through allocation. Consumer:
`seq_clamp_patch_voices()` (`components/synth_core/sequencer_core/seq_core_synth.c`).
Not posted upstream.

### `instrument.c` - `instrument_get_num_voices()` clamps the voice-list copy

The copy into the caller's array is capped at `MAX_VOICES_PER_INSTRUMENT`
(and the clamped count returned). Every caller passes an array of that size
and a configured instrument never exceeds it, but the filter scope calls this
from the UI task without the queue lock while the render task can release
the instrument; a torn `num_voices` read from freed memory would otherwise
write past the caller's stack array. Not posted upstream; upstream's copy
(v1.2.188) is unbounded.

### `CMakeLists.txt` - component build settings

The component's `CMakeLists.txt` is local (upstream ships no ESP-IDF
component files). Besides the Kconfig flags in the entries above it:
- globs `src/*.c` and excludes `amy-example.c`, `libminiaudio-audio.c` and
  `usb.c` (this project has its own USB stack);
- `-DNDEBUG` on the five DSP files with no structural state (`filters.c`,
  `oscillators.c`, `envelope.c`, `delay.c`, `log2_exp2.c`,
  `AMY_DSP_HOT_FILES`), so the globally enabled asserts drop out of their
  per-sample loops; `amy.c` keeps its asserts;
- `-Wno-strict-aliasing` on `amy.c`;
- option `AMYSYNTH_AMY_O3` (default off) compiles the component at `-O3`.

## Deferred / needs porting

| Edit | Status |
|------|--------|
| Block-processed ESP32 stereo reverb (`delay.c`, `#ifdef ESP_PLATFORM`) | **Not applied.** Upstream changed `stereo_reverb()` to take `reverb_params_t *rev` (all delay state inside struct); the block-processed optimization needs adapting to the new API before it can be reapplied. The locals-caching optimization (now upstream via #811) recovers part of the same win on the new API; re-evaluate whether full block processing is still worth it after hardware measurement. |

### Upstream changes after v1.2.163 that meet local edits at the next sync

Upstream `main` at v1.2.188:
- #1159, #1163, #1171 (v1.2.171, v1.2.169, v1.2.175): carry the chorus sweep, the filter-type reset and the sized cubic / FM-256 kernels; those local copies retire.
- Tail-first chain rendering (`7396b3c`, PR #1200, v1.2.186): `render_osc_wave()` renders the chained osc first, and an osc's distortion and filter now process everything chained below it, not only its own wave; the SILENT-head special case is gone. No local hunk touches `render_osc_wave()`.
- Sequencer accumulate-on-tag (#1156, `0c05eba`, v1.2.166): events sent on a tag are added instead of replacing the tag's event, `ticks="0,0,<tag>"` (`sequencer_clear_tag()`) clears a tag, and `sequences[]` becomes a slot pool with a `tag` field. The periodic-entry horizon hunk and the `ram_caps_sequencer` per-fire hunk in `sequencer.c` index `sequences[tag]` and do not apply as written.
- Render lock (#1205, `8e285d7`, v1.2.188): `amy_grab_render_lock()` / `amy_release_render_lock()`, recursive per thread and taken before the queue lock, held per block by `amy_simple_fill_buffer()` and the `i2s.c` render paths and around the flush in `amy_execute_deltas()` and before a patch load. Upstream's `amy_render()` takes no lock; the local render lock holds `amy_queue_lock` inside it.
- No osc allocation on ingest (#1190, `b2f9928`, v1.2.181): `amy_event_to_deltas_queue()` no longer allocates oscs; `play_delta()` allocates them on the render thread under the queue lock. This removes the ingest-side `alloc_osc()` / `reset_osc()` race named in the render-lock entry.

---

# AMY Local Edits

Track local, project-specific changes made against the upstream AMY component here.

## 2026-09-05 — Upstream sync v1.2.160 -> v1.2.163

- **Vendor sync** of `components/amy` to upstream `0fb0a00` (v1.2.163, 12
  commits since v1.2.160, three PRs); `amy/` submodule gitlink bumped to match.
  - **Method:** upstream's delta applied directly (`git diff a89df0c
    0fb0a00 | git apply`) - the seven files it touches carry no local hunk,
    so the result equals an overlay rebase with zero conflicts. Every local
    edit is untouched.
  - **Upstream behavior new to this build:** #1149 restores real osc counts
    for the drum-kit patches in `patch_oscs[]` (258 and 384 -> 38, 385-390
    -> 42; they were placeholder 1), so loading a kit sizes its osc block
    right on the first allocation. The firmware never routes those patch
    numbers, so nothing changes at runtime here; through
    `amy_patch_oscs_per_voice()` the polyphony clamp would now hold a kit
    patch to one voice. #1143/#1145 add `amy.version` to the Python module
    (not built here).
  - **Retired:** none. **Kept:** everything in the diagram at the top of
    this file.
  - **Verification:** `build_project` green; `AMY_SAMPLE_RATE` ESP branch
    confirmed 48000. No new HW items: nothing the firmware executes changed.

## 2026-08-28 — Upstream sync v1.2.145 -> v1.2.160

- **Full vendor sync** of `components/amy` to upstream `a89df0c` (v1.2.160, 67
  commits since v1.2.145); `amy/` submodule gitlink bumped to match.
  - **Method:** the standard overlay-rebase, with the vendored distortion port
    stripped from the overlay first - upstream merged #1134 in a different
    (post-review) shape, so every dist hunk resolved to "take upstream" and
    stripping avoided the leftover sweep. New vendor base = upstream `main` as
    is; no PR pre-merges.
  - **Retired** (see the Dropped table): per-osc distortion #1116, the dist
    follow-ups #1134, `amy_update_handle` init registration #1135, the
    second-core render semaphore #1137. #1106/#1107 were already in the
    previous base and are upstream since v1.2.149.
  - **Kept:** everything in the diagram at the top of this file. The PCM
    retrig fade-restart moved inside #1129's `want_stretch`/`fresh_start`
    branch of `pcm_note_on()`; every other edit rebased unchanged.
  - **First-party fallout:** `fx_push_dist()` (`components/synth_core/amy_fx.c`)
    rewritten to the scope-by-event rule - the shared `dist_*` event fields
    with no osc named, so the event lands at bus scope. `voice_config.c`
    already used the per-osc shape and is unchanged.
  - **Upstream behavior new to this build:** #1127 drops osc references that
    reach outside the voice being configured, #1123 rechecks `synth[]` before
    following osc references, #1129 adds `sample_offset`/`fit` and grows
    `synthinfo` by roughly 56 B per osc in the PSRAM arena.
  - **Verification:** `build_project` green; `AMY_SAMPLE_RATE` ESP branch
    confirmed 48000. Residual over upstream, re-measured 2026-09-05 after the
    lock-prototype retire: 14 src files, +410/-58, plus `Kconfig` and
    `CMakeLists.txt` with no upstream counterpart. HW verify pending.

## 2026-08-07 — `amy_patch_oscs_per_voice()` read accessor

- **`src/patches.c` + `src/amy.h`** (commit 30264d3): returns a patch's
  oscs-per-voice (built-in `patch_oscs[]` table or `memory_patch_oscs[]` for
  user patches; 0 for undefined/reserved numbers) so an embedder can budget
  `num_voices` BEFORE a load instead of discovering exhaustion via "cannot
  find N oscs" partway through allocation. Purely additive read API, same
  family as `amy_voice_base_osc()`. **Upstream PR candidate.** Consumer:
  `seq_clamp_patch_voices()` (seq_core_synth.c, `SEQ_TRACK_OSC_BUDGET` 32).

## 2026-08-07 — PSRAM fallback for wire-string allocations (post-sync fix)

- **`src/api.c` + `src/parse.c` + `src/sequencer.c`** (commit 8a66040): since
  v1.2.121 every scheduled event, stored sequence entry, and periodic-fire
  working copy is malloc'd from `ram_caps_events` - the pool `alloc_osc`
  also draws per-osc synth state from (pinned internal in main.c). One
  oversized patch load (built-in piano layer-wide = 400 oscs attempted vs
  ~76 KB internal free) exhausts it; every scheduled event AND tag cancel
  after that is silently dropped (print-once OOM already consumed), so
  pause stops pausing and re-emits stay mute. Fallback to
  `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT` at the three wire-string sites -
  control-path text, PSRAM latency irrelevant. Same policy family as the
  delta-pool spill. HW verify pending.
- **SUPERSEDED same day by `ram_caps_sequencer`** (below): the hardcoded
  PSRAM fallback became a proper config knob; the three sites now allocate
  from `ram_caps_sequencer` first with `ram_caps_events` as the fallback.

## 2026-08-11 — `ram_caps_oscs` config knob (PSRAM osc-state arena)

- **`src/amy.h`** (amy_config_t) + **`src/api.c`** (`amy_default_config`) +
  **`src/amy.c`** (`alloc_osc`): new `ram_caps_oscs` config field for the
  per-osc `synthinfo`/`mod_synthinfo`(+breakpoints) blocks that `alloc_osc`
  grows on demand. Defaults to `ram_caps_events` (upstream-compatible
  no-op). Motivation: upstream conflates the hot delta-pool caps with osc
  state; our `main/main.c` pins `ram_caps_events` internal for delta
  latency, so one apply of the 25-osc/voice built-in piano grew the
  never-shrinking osc arena by ~46 KB of INTERNAL heap (54 KB -> 7.5 KB
  free, harness-measured 2026-08-11, reboot-only recovery since `free_osc`
  is reachable only from full reset). `main/main.c` now points
  `ram_caps_oscs` at `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`: the blocks are
  per-BLOCK control state, not per-sample buffers (those stay internal via
  `ram_caps_fbl`/`ram_caps_block`).
  Merged upstream as #1107 (v1.2.149) and no longer a local edit; `main.c`
  keeps pointing the field at PSRAM.

## 2026-08-07 — `ram_caps_sequencer` config knob (PSRAM-first wire storage)

- **`src/amy.h`** (amy_config_t) + **`src/api.c`** (`amy_default_config`,
  `amy_add_event`) + **`src/parse.c`** (`handle_ticks_message`) +
  **`src/sequencer.c`** (periodic-fire copy): new `ram_caps_sequencer`
  config field for stored sequencer wire strings and their serialize/fire
  buffers. Defaults to `ram_caps_events` (upstream-compatible no-op); the
  three allocation sites try it first and fall back to `ram_caps_events`.
  `main/main.c` points it at `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`: wire
  strings are cold control-plane data (one us-scale parse per fire), so
  PSRAM-first permanently decouples the schedule/cancel path from the
  ~76 KB internal pool that `alloc_osc` draws per-osc synth state from,
  and ends variable-length string churn in that pool.
  **Upstream PR candidate** (fits the existing `ram_caps_*` family; pairs
  with `amy_patch_oscs_per_voice`). HW verify pending.

## 2026-08-07 — Upstream sync v1.2.104 -> v1.2.121

- **Full vendor sync** of `components/amy` to upstream `85a7025` (v1.2.121, 94
  commits since v1.2.104); `amy/` submodule gitlink bumped to match.
  - **Method:** same scratch-clone overlay-rebase as the July sync (whole
    vendored tree committed as one overlay on `fd09bd2`, rebased onto
    `85a7025`). Surviving delta vs upstream: 214+/47- across 12 files (was
    ~456 changed lines vs 1.2.104).
  - **Retired** (see the Dropped table): FILTER_NOTCH #1000, FILTER_PHASER
    #1020, SEQ_LOCK + active-tag dense index + int16 shrink (upstream
    sequencer rework), UART MIDI poll guard, task-priority `- 1` fix.
  - **Kept** (reapplied over v1.2.121): 48 kHz ESP branch, fixed-point
    Kconfig gate + `ldexpf`, render lock + lock prototypes, PIE render-clear
    extension + `malloc_caps_block` call sites, delta-pool PSRAM spill,
    COARSE profiler, ingest/tick split (`flush_due_deltas`, open PR #1049),
    `amy_voice_base_osc()` + `instrument_get_num_voices()` clamp,
    `amy_gamma9001_pcm_bytes()`, IDF 6.0 task signatures, dead dual-core
    bus-sum skip, residual IRAM annotations (now filters.c x2 +
    oscillators.c x1), OOM-guard family. New minimal edit: `sequencer_init`
    NULL guard (upstream's rewritten init writes through an unchecked
    `malloc_caps`; on failure the sequencer now disables wholesale via the
    existing `sequences == NULL` convention instead of crashing).
  - **First-party API drift ported:** `amy_event.sequence[SEQUENCE_*]` ->
    `ticks[TICKS_*]` (events are now serialized to wire strings and handed to
    `sequencer_add_wire`; the tag-cancel form tick=0/period=0 survives the
    round trip because 0 is a set value and `_EPRINT_U_SEQ` emits through the
    last set element). 9 sites in `components/synth_core/amy_helpers.c` +
    `sequencer_core/seq_core_engine.c`.
  - **Verification:** `build_project` green; `AMY_SAMPLE_RATE` ESP branch
    confirmed 48000; `amy_render` re-checked in the ELF: 3x `loopnez` +
    3x `ee.vst.128.ip`, PIE wrapper still fully inlined, no standalone
    `amy_block_zero_blocks` symbol. HW verify pending (sequencer core was
    fully replaced upstream - timing + decorated-trig patterns are the
    priority checks).

## 2026-08-04

- **Keep the sequencer tick service out of event ingest** (`components/amy/src/amy.c`)
  - **What:** the due-delta flush (lock, play deltas whose time has arrived, unlock) is split out of `amy_execute_deltas()` into a static `flush_due_deltas()`, and the pre-patch-load call in `amy_event_to_deltas_queue()` now calls the flush instead of the full function. `amy_execute_deltas()` itself is unchanged in behaviour (tick service + CV poll + flush) and keeps running in rendering contexts (`amy_render_audio()`, `amy_simple_fill_buffer()`).
  - **Motivation:** `amy_event_to_deltas_queue()` runs in whatever thread sent the event, so any patch/voice event executed the sequencer tick service (`sequencer_check_and_fill()`) in that thread. Two concrete defects: (1) the tick decision is an unguarded 64-bit read-modify-write of `amy_global.next_amy_tick_us`, so an ingest-thread call racing the audio thread's per-block call can process the same tick twice (audible double step-advance) or tear the accumulator; (2) the app's `amy_external_sequencer_hook` runs on a thread the app never registered for it - this project's hook (`sequencer_core_service_tick()`) reads sequencer layer state lock-free under a documented "render task only" contract, and it was the root entanglement behind the decorated-step re-entry assert family (an event *send* could re-enter event *emission*). The flush before a patch load only ever wanted "settle pending deltas so the load sees applied state" - that half is thread-agnostic (everything under `amy_queue_lock`) and is all that remains on the ingest path.
  - **Risk:** Low. Rendering-context behaviour is bit-identical; the only change is that a patch load no longer advances the sequencer clock early from the sender's thread - ticks now advance solely at the per-block cadence, which is the designed clock. A tick that would have fired during the load fires at most one block (5.33 ms) later, exactly as it does when no event is in flight.
  - **Upstream:** merged as #1049 (v1.2.122), so this is upstream code now, not a local edit - see the Dropped table. Framing that carried it: "event ingest should not run the sequencer".
  - **Rollback:** revert the call in `amy_event_to_deltas_queue()` to `amy_execute_deltas()` and fold `flush_due_deltas()` back into it.

- **PCM index fractional bits 8 -> 12** (`components/amy/src/pcm.c`; merged to Sync 2026-08-07 via e1f40d7. **RETIRED 2026-08-09**: superseded by upstream #1064 (`42a1876b2`, merged 1.2.121+1), adopted into the vendor tree - dpwe's approach computes the step at 16 fractional bits via a block-local phase (`PCM_INDEX_STEP_EXTRA_BITS`) while keeping S23.8 storage, so there is no length cap; the fold-back is lossless at our 256-sample block. Both hunks (FRAC_BITS 12, pcm_load length guard) reverted to upstream. The declick compensator was re-threaded into the restructured loop; its past-end-of-sample decay path was dropped in favor of upstream's `break` - with 1.2.121's hard-stop note-offs the compensator can only ring within ~1.3 ms of a retrigger, never at natural sample end.)
  - **What:** `PCM_INDEX_FRAC_BITS` 8 -> 12 (phase split 23.8 -> 19.12), plus a `pcm_load()` length guard rejecting samples longer than the 19-bit index (524287 frames, ~10.9 s at 48 kHz; longest current preset is 114688 frames).
  - **Motivation:** with 8 bits the playback step quantizes to 1/256 frame. The ROM 808 bank is 22050 Hz on a 48 kHz engine, so native pitch is already ratio 0.459 and the drum floor (MIDI 24) is ratio 0.057 - host-sim-measured detune: -8.9 cents at native pitch, -84.5 cents at the floor, with representable pitches ~90-120 cents apart at the bottom. 12 bits measures <=2 cents everywhere; render cost is shift-immediates only.
  - **Risk:** Low. Steady-state SNR unchanged (interp noise sits ~35 dB below the amp-path rounding floor either way). API note: `trigger_phase` PCM start-frame scaling shifts 16x.
  - **Upstream:** PR candidate in the #951 mold; the measured detune table is the pitch.
  - **Rollback:** revert both hunks.

- **PCM retrigger declick** (`components/amy/src/pcm.c`, `amy.h`, `amy.c`; merged to Sync 2026-08-07 via e1f40d7. The note-off arm was retired in that merge: upstream 1.2.121 replaced pcm_note_off phase-seek with an immediate mode-based osc stop, so there is no longer a rendered tail for the compensator to cancel on note-off - the retrigger arm, the primary fix, is unchanged)
  - **What:** re-onset of a sounding PCM osc (and note-off of a non-looping one) snaps the phase, stepping the output from wherever the old tail was. The step is now cancelled by an exponentially decaying compensator: `pcm_note_on/off` arm `pcm_declick += pcm_last_out`; `render_pcm` adds it per sample and decays it (`declick -= declick >> PCM_DECLICK_SHIFT`, shift 6 = tau 1.3 ms at 48 kHz, audible decay within one block), with a sub-audible snap-to-zero so the shift decay terminates. Two `SAMPLE` fields added to `struct synthinfo`, zeroed in `reset_osc_state()`.
  - **Motivation:** long PCM tails (deliberately not note-off gated here) mean sequencer retriggers land mid-tail: host-measured ~7500-LSB discontinuities against ~800 natural slew at the drum floor - a hard click every step, growing as pitch drops. Mono choke is kept (matches a real 808's continuous analog voice); only the step is removed. Host-sim-verified: jump-at-retrigger falls to the natural-slew floor at every pitch, steady-state pitch/SNR table byte-identical.
  - **Risk:** Low. One MAC per rendered PCM sample; compensator is nonzero only ~100 samples after a phase jump. No effect on first onsets, looping, or file-streamed PCM (file path never arms it).
  - **Upstream:** PR candidate ("declick PCM retrigger") - every embedder retriggering PCM has this step; pairs with the frac-bits PR.
  - **Rollback:** revert the three-file commit (`dcfab63`).

## 2026-07-28 — Upstream sync v1.2.31 -> v1.2.104

- **Full vendor sync** of `components/amy` to upstream `fd09bd2` (v1.2.104, 358
  commits / ~80 PRs since v1.2.31), and the `amy/` submodule now tracks
  upstream `main` (`.gitmodules` `branch = main`, gitlink at `fd09bd2`).
  - **Method:** scratch clone of the submodule; the whole vendored tree was
    committed as one overlay on `1e23c70`, then rebased onto `fd09bd2` -
    conflicts resolved per predecided retire/keep lists.
  - **Retired** (arrived with upstream, see the Dropped table): LUT trig
    #875/#877 + `qsin_fxpt_lutable`, float suffixes `470a6c0`, `SMUL64R` +
    fixedzeros kernels #951, mod_source cycle guard #949, integer sysclock
    #827 + 64-bit clock/wrap #982 (also takes the µs-domain tick compare and
    single-precision tempo math), `AMY_IRAM_ATTR`/`AMY_DRAM_ATTR` macros +
    most hot-path annotations + clipping-LUT DRAM placement #905/#907,
    aligned FM scratch #967/#969, cv_trigger/envelope/interp_partials OOM
    shapes (#961 family, `role`-field spellings).
  - **Kept** (reapplied over v1.2.104): everything in the diagram at the top
    of this file - 48 kHz ESP branch, fixed-point Kconfig gate + `ldexpf`,
    render lock + narrowed lock prototypes, PIE render-clear extension,
    delta-pool PSRAM spill + no-abort cap, SEQ_LOCK + int16 active-tag index,
    COARSE profiler, FILTER_NOTCH, `amy_voice_base_osc()` +
    `instrument_get_num_voices()` clamp, `amy_gamma9001_pcm_bytes()`, IDF 6.0
    task signatures + UART poll guard, three residual IRAM annotations.
  - **Verification:** `build_project` green first try (no first-party API
    breaks surfaced); `AMY_SAMPLE_RATE` ESP branch confirmed 48000;
    `amy_render` re-checked in the ELF: 3x `loopnez` + 3x `ee.vst.128.ip`,
    PIE wrapper still fully inlined. HW verify pending. Semantic upstream
    changes (per-instrument `synth_level`, `role` field split, drum kit
    restructure #913, bus selection #858/#931, note chaining #947,
    `r`/`zA` deprecations) are build-verified only - listen for behavior
    drift on first hardware session.

## 2026-07-28

- **New read accessor: `amy_voice_base_osc()`** (`components/amy/src/patches.c`, prototype in `amy.h`)
  - **What:** `bool amy_voice_base_osc(uint16_t voice, uint16_t *base_osc)` returns the base oscillator of a voice, or false when the table is uninitialised, the voice index is out of range, or the entry is unset.
  - **Motivation:** the `voice_to_base_osc` table is a plain global with no declaration in any header, so an embedder that needs a voice's oscillators has no supported way to ask. The filter scope (live modulated-cutoff overlay in the filter editor) has to map an edit target's synth slot to the oscillators actually carrying its filter. `instrument_get_num_voices()` is already public and supplies the voice list, but the voice -> osc half of that mapping was unreachable.
  - **Risk:** Very low. Purely additive, read-only, bounds- and NULL-checked; no existing code path changes behaviour.
  - **Upstream:** good PR candidate - the accessor is generic, not ESP-specific, and nothing about it depends on this project.
  - **Rollback:** delete the function from `patches.c` and its prototype from `amy.h`.

- **Hardening: clamp the voice-list copy in `instrument_get_num_voices()`** (`components/amy/src/instrument.c`)
  - **What:** the copy loop now caps `num_voices` at `MAX_VOICES_PER_INSTRUMENT` before writing into the caller's buffer (and returns the clamped count).
  - **Motivation:** the function copies `instrument->num_voices` entries into the caller's array with no bound, and every caller passes a `MAX_VOICES_PER_INSTRUMENT`-sized stack buffer. A legitimately configured instrument never exceeds that, but the filter scope now calls this from the UI task without the queue lock while the render task can release and recycle the instrument struct; a torn read of `num_voices` (a `uint8_t`, so up to 255) from freed memory would drive an unbounded write into a 32-entry stack array - a UI-task stack smash. With the clamp, the worst outcome of the race is a wrong-but-bounded voice list for one frame, which the scope's downstream bounds- and NULL-checks already tolerate.
  - **Risk:** Very low. Behaviour is unchanged for every legitimate input; the clamp only bites on values that were already impossible to satisfy safely.
  - **Upstream:** defensible as plain API hardening (the implied buffer contract is `MAX_VOICES_PER_INSTRUMENT`), though the racing caller is this project's; could ride along with an `amy_voice_base_osc()` PR.
  - **Rollback:** remove the clamp above the copy loop.

- **Upstream cherry-pick: float-suffix the biquad coefficient generators** (`components/amy/src/filters.c`, upstream `470a6c0`, released 1.2.55)
  - **What:** the `filters.c` subset of upstream's FP64-literal sweep: `LOWEST_RATIO`, the `qFactor`/`f` clamps in `dsps_biquad_gen_{lpf,hpf,bpf}_f32`, the `(float)M_PI` cast in the LPF floor, and the dead pole-limit block's `0.99f`. The same treatment is applied to the local `dsps_biquad_gen_notch_f32`, which postdates the upstream fix.
  - **Motivation:** unsuffixed double literals promote the clamp comparisons and the `M_PI` expression to software FP64 on the S3. asmdiff (esp-15.2.0, `-O2`): before, each generator called `__extendsfdf2` + `__ltdf2`/`__ledf2` + `__gtdf2`; after, zero FP64 libcalls and the three shared generators match upstream main's codegen exactly (106/100/101 insns). Control-rate cost (once per filtered osc per block), so a small win - but free.
  - **Risk:** None measurable. Upstream verified the full sweep bit-for-bit against all 104 reference tests; these literals (0.51, 0.45, 0.0001, 0.99) are exactly representable in binary32, so the clamped values are unchanged.
  - **Retire:** on the next vendor sync >= 1.2.55 (the notch generator's suffixes ride with the notch edit).
  - **Rollback:** strip the `f` suffixes and the `(float)` cast.

## 2026-07-25

- **Realigned the OOM-guard family to merged upstream #961** (`components/amy/src/amy.c`, `amy.h`, `instrument.c`, `cv_trigger.c`, `interp_partials.c`)
  - **What:** the three 2026-07-23 entries below were the first cut of this work; the upstream PR is the iterated version of the same concept, so the vendored tree now carries the merged shapes verbatim instead of its own. Adopted: `amy_oom(fmt, ...)` as the single reporting funnel (counts into `amy_global.oom_count`, logs, `abort()`s under `AMY_DEBUG` so host runs stop at the cause) with the pollable `amy_get_oom_count()` API; `ensure_osc_allocd()` returning `bool` so every caller tests the result instead of re-checking `synth[osc] == NULL` after the call; the breakpoint realloc allocating the replacement *before* freeing the old block, restoring the old pointers on failure.
  - **Why it matters beyond tidiness:** the local free-then-alloc realloc had a hole the call-site guards could not cover - once `alloc_osc()` was allowed to leave `synth[osc]` NULL, `ensure_osc_allocd()` dereferenced it itself while copying the saved vector pointers, so an OOM during breakpoint growth crashed inside the function, before `play_delta`'s guard ran. Upstream's version also stops seeding `new_max_num_breakpoints[]` with `DEFAULT_NUM_BREAKPOINTS`: a set already grown past 8 was silently shrunk while the copy loop still copied its old (larger) contents, overrunning the new vector. Both are gone.
  - **Kept local (deliberately not upstream):** `malloc_caps_block()` PIE alignment on `delay_mod`; the delta-pool PSRAM spill and its non-aborting `MAX_DELTA_BLOCKS` path (upstream `abort()`s; on target a dropped event beats taking the synth down) - now reported through `amy_oom()` so it lands in the same counter. Retired: the private `amy_delta_drop_count`, superseded by `oom_count`. Still not applicable: the `midi_mappings.c` yield-state guard (that allocation does not exist in v1.2.31).
  - **Risk:** Low, and the same cold control-path branches as before. The behavioral difference under OOM is strictly better: a failed breakpoint grow now keeps the osc playing with its old envelope instead of freeing it.
  - **Rollback:** revert this commit; the pre-realignment shapes are the three 2026-07-23 entries below.
  - **Verification:** `build_project` green. Diff shapes were tested during the upstream PR; no further host-sim run made here. HW verify pending (rides with the `feat/ble-midi` verify).

## 2026-07-23

> Superseded 2026-07-25 by the realignment entry above - these describe the pre-#961 local shapes, kept for the root-cause history.

- **Bug fix (OOM crash class): `alloc_osc()` and downstream NULL guards** (`components/amy/src/amy.c`)
  - **Root cause:** `alloc_osc()` never checked the `malloc_caps()` result before writing through it — internal-heap exhaustion (532 B/osc in `ram_caps_events`, now user-drivable via chord-preset voice widening) was a guaranteed `StoreProhibited`. Same unchecked-malloc family as the delta-pool and reverb OOM crashes fixed earlier. Running out of osc *slots* (`max_oscs`) was already handled gracefully upstream; running out of *heap* for an osc was not.
  - **Fix:** `alloc_osc()` logs and leaves `synth[osc]`/`msynth[osc]` NULL on failure (render loop and `reset_osc()` already tolerate NULL). Guards added at the deref funnels: `play_delta()` entry drops the delta for a NULL osc; the breakpoint-realloc site re-checks (the realloc frees the old osc first, so the entry guard's promise can be invalidated mid-function); `chained_osc_would_cause_loop()` / `mod_osc_would_cause_loop()` refuse the link when the target osc is NULL (keeps NULL out of the chain/mod walks render follows); the `ALGO_SOURCE` branch unsets a source whose osc failed to allocate. The residual unguarded `ensure_osc_allocd` callers flagged by the internal-DRAM audit are closed by the sweep-completion entry below.
  - **Risk:** Low. All guards are cold compare-branches on control paths (delta application, link validation), none per-sample. Behavior under OOM changes from crash to silent voice + stderr log.
  - **Rollback:** Remove the five `LOCAL EDIT` blocks (alloc_osc guard, play_delta entry + realloc guards, two loop-helper guards, ALGO_SOURCE else-branch).
  - **Upstream PR candidate:** yes — universal logic bug, platform-independent.

- **Memory: active-tag index arrays `int32_t` → `int16_t` + init OOM guard** (`components/amy/src/sequencer.c`)
  - **What:** `s_active_tags[]` and `s_tag_slot[]` (the O(1) active-tag scan LOCAL EDIT) hold tag numbers / list indices < `max_sequences`; int16 halves their internal-RAM cost — with 1730 tags, per-tag cost drops 20 B → 16 B, reclaiming ~6.8 KB. `sequencer_init()` clamps `max_sequencer_tags` to 32766 so values and the −1 sentinel always fit, and now NULL-checks its three allocations (on failure the sequencer disables wholesale: `max_sequences = 0`, `sequencer_add_event` rejects everything via a new `sequences == NULL` guard, the tick scans nothing — no crash).
  - **Risk:** Negligible. Same algorithm, narrower storage; casts are total (clamped range). Boot-time OOM path is new but strictly safer than the previous NULL deref.
  - **Rollback:** Revert the two array types to `int32_t` (and the `sizeof`s), drop the clamp and the init/add-event NULL guards.
  - Companion (not an AMY edit): `main/main.c` `max_sequencer_tags` trimmed 1760 → 1730 (top tag 1727 + off-by-one margin).

- **Bug fix (OOM crash class, follow-up): `instrument_init()` / `voice_fifo_init()` NULL guards** (`components/amy/src/instrument.c`)
  - **Root cause:** found while host-proving the alloc_osc guards against upstream (the OOM repro segfaulted in the per-synth `instrument_info` malloc after the osc path was hardened). Both allocations wrote through an unchecked `malloc_caps()` result — same family as alloc_osc, on the same patch-load path, ~248 B + 2×40 B internal per synth creation.
  - **Fix:** `voice_fifo_init` and `instrument_init` return NULL on OOM (with partial-alloc unwind); a NULL `instruments[n]` is already the module's "synth not defined" state (`instrument_number_exists`), so every downstream consumer degrades to a logged no-op.
  - **Not backported:** the midi yield-state guard applied upstream (`midi_mappings.c`) — that allocation doesn't exist in v1.2.31, and this build runs `AMY_MIDI_IS_NONE`. Delta-pool guards were already local (2026-06-27).
  - **Risk:** Negligible; cold branches on synth creation only.
  - **Rollback:** Remove the three `LOCAL EDIT` blocks in instrument.c.
  - **Upstream PR:** included in the fork's `fix/alloc-osc-oom-guards` branch ("Guard runtime allocations against out-of-memory"), together with the alloc_osc, delta-pool, and midi yield-state guards.

- **Bug fix (OOM crash class, sweep completion): remaining `ensure_osc_allocd` deref sites** (`components/amy/src/amy.c`, `cv_trigger.c`, `interp_partials.c`)
  - **Root cause:** with `alloc_osc()` allowed to leave `synth[osc]` NULL, every caller that dereferences immediately after `ensure_osc_allocd` moves the OOM crash to itself instead of preventing it. Audit found five such sites outside the funnels guarded above: `config_chorus` (LFO osc, plus a pre-existing NULL-line deref when `alloc_chorus_delay_lines` fails), the chorus LFO block in the render loop, `set_cv_from_osc`, and the PARTIALS / INTERP_PARTIALS note-on paths.
  - **Fix:** `config_chorus` forces the chorus off and returns on either failed alloc (mirrors `config_reverb`'s merged OOM pattern); `alloc_chorus_delay_lines` counts a NULL `delay_mod` as failure so partial allocs are torn down (the render loop block-clears `delay_mod` whenever level ≠ 0); the render loop skips a bus whose chorus LFO osc is NULL; `set_cv_from_osc` unsets the CV mapping (`osc_for_cv = -1`, the module's existing "unset" value); PARTIALS note-on skips NULL partials and note-off tolerates the gap; INTERP_PARTIALS note-on drops the note before the harmonic loops that assume all partials exist. The two `ensure_osc_allocd` calls in the event-to-delta path stay guard-free on purpose: they don't dereference locally, and the deref funnel (`play_delta`) is already guarded.
  - **Risk:** Low. Cold branches on config/note-on paths; the only render-path change is a NULL compare per bus per block, and it sits next to an `ensure_osc_allocd` call that was already there.
  - **Rollback:** Remove the eight `LOCAL EDIT (S3-Amysynth): OOM guard` blocks across the three files.
  - **Upstream PR:** folded into `fix/alloc-osc-oom-guards` (same shapes, ported to v1.2.87's `role` field naming). Host verification there: OOM repro survives (exit 0) and the `amy.test` suite output is byte-identical to pristine upstream main.

## 2026-06-20 - Not Active

- **Performance: block processing / vectorization in `stereo_reverb()`** (`components/amy/src/delay.c`)
  - **What:** Replaced the sample-by-sample 10-delay-line reverb loop with a vectorized version when `ESP_PLATFORM` is defined. The upstream loop causes heavy register spilling due to interleaving 10 delay-line pointers and 4 LPF states for every sample.
  - **How:** Allocated two static `SAMPLE[256]` block buffers in `.bss`. Split the 6 early reflections (`ref_1..6`) and the 4-line reverb matrix into independent `n_samples` loops. Fixed an upstream bug where `f3state` was reused for `d4`'s LPF instead of `f4state` inside the new block path.
  - **Risk:** Low. The `multicore=0` environment makes static block buffers safe without mutexes. Purely an algorithmic reorganization; the math sequence is unchanged. Guarded heavily by `#ifdef ESP_PLATFORM` so upstream non-ESP builds remain unaffected.
  - **Rollback:** Remove the `#ifdef ESP_PLATFORM` block in `stereo_reverb()` and restore the pure `while(n_samples--)` upstream loop from the `#else` branch.

## 2026-06-19

- **Performance: pin the per-sample clipping LUT to internal DRAM (`AMY_DRAM_ATTR`).**
  - **What:** `clipping_lookup_table` (`components/amy/src/clipping_lookup_table.h`,
    `const uint16_t[NONLIN_RANGE]` = 4914 entries ≈ 9.6 KB) is read for **every output
    sample** in `amy_fill_buffer` (`amy.c` soft-clip stage, ~line 1813). In flash
    `.rodata` it is served via the PSRAM XIP cache (`CONFIG_SPIRAM_RODATA=y`), so the
    inner output loop can take cache-miss stalls. Moved it to fast internal DRAM.
  - **How:** New `AMY_DRAM_ATTR` macro in `amy.h` (mirrors the existing `AMY_IRAM_ATTR`):
    `= DRAM_ATTR` on `ESP_PLATFORM` (after the already-present `#include <esp_attr.h>`),
    no-op elsewhere. Applied to the table declaration as
    `const uint16_t clipping_lookup_table[NONLIN_RANGE] AMY_DRAM_ATTR PROGMEM = {`.
    `DRAM_ATTR` (data section), **not** `IRAM_ATTR` — IRAM is instruction memory and a
    `uint16_t` table needs word-safe data placement. `PROGMEM` is empty on ESP, kept for
    upstream portability. Edit sites marked inline `// LOCAL EDIT (... 2026-06-19) ...`.
  - **Verified:** `.dram0.data` grew ~+9.8 KB (16,954 → 26,778 B); the table left flash
    rodata. Affordable only because the ring-buffer move below freed ~64 KB internal first.
  - **Risk:** Low. Pure placement change; the table is `const`, never written. Costs ~9.6 KB
    internal DRAM (covered by the freed ring-buffer space).
  - **Rollback:** Remove `AMY_DRAM_ATTR` from the table declaration; optionally remove the
    `AMY_DRAM_ATTR` macro block in `amy.h`.

- **Non-AMY-source change (in `components/usb_audio/usb_audio.c`):** `s_ring_buffer`
  (`int16_t[RING_BUFFER_SIZE]` = 32768 = **64 KB**) was a static array in internal DRAM
  `.bss`, consuming a large share of the scarce internal SRAM. Converted to a pointer
  allocated from PSRAM via `heap_caps_malloc(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` in
  `usb_audio_init()` (with NULL-check + mutex cleanup on failure). The buffer is accessed at
  USB-frame / render-block granularity (memcpy chunks + mutex-guarded per-sample writes), not
  in the per-sample DSP inner loop, so PSRAM latency is irrelevant. Verified: `.dram0.bss`
  dropped 81,072 → 15,536 B (−64 KB). Net internal SRAM change for both edits: ≈ −55 KB used.

- **Bug fix (crash): reverb delay-line OOM caused NULL-deref panic (`LoadProhibited`).**

  > Superseded: merged upstream as [#744](https://github.com/shorepine/amy/pull/744)
  > and since reshaped there - the reverb state now lives in `reverb_params_t` and
  > rolls back through `deinit_stereo_reverb()`, with no `free_stereo_reverb()`,
  > `stereo_reverb_ready()` or `amy_reverb_alloc_failed()`. Nothing below is still a
  > local edit; kept for the root-cause history.

  - **Symptom:** Raising **Reverb** from 0 in the menu crashed with
    `Guru Meditation Error: Core 1 panic'ed (LoadProhibited)`, `EXCVADDR=0x00000000`,
    in `stereo_reverb` (via `amy_fill_buffer` / `amy_render_audio` / `amy_update` /
    `amy_usb_render_task`), preceded by `unable to alloc delay line of 4096 samples`.
  - **Root cause:** `init_stereo_reverb()` allocates ~10 delay lines via
    `new_delay_line(..., ram_caps_delay)`. When those allocations fail, the
    `delay_1..ref_6` pointers stay NULL, but `config_reverb()` still committed a
    nonzero `reverb.level`, and the render guard in `amy_fill_buffer` checked **only**
    `reverb.level > 0` — so `stereo_reverb()` ran and dereferenced NULL via
    `DEL_IN(ref_1, ...)`. Echo never crashed because its render guard already checks
    `echo_delay_lines[0] != NULL`; reverb had no equivalent guard. This is a strict
    upstream robustness bug: a failed allocation should disable the effect, not crash.
    (The underlying OOM trigger — `ram_caps_delay` pinned to internal SRAM — is fixed
    separately in `main/main.c`; see the non-AMY note below.)
  - **Fix (AMY source):**
    - `delay.c` / `delay.h`: `init_stereo_reverb()` return type `void` → `bool`. On any
      `new_delay_line()` failure it logs, frees every partial allocation
      (`free_stereo_reverb()`), leaves all pointers NULL, and returns false. New
      `stereo_reverb_ready()` returns `delay_1 != NULL` (init guarantees all-or-nothing).
    - `amy.c` `config_reverb()`: only enables reverb (commits nonzero level + calls
      `config_stereo_reverb`) when `init_stereo_reverb()` succeeds; on failure it forces
      `reverb.level = 0` and sets the new `reverb.alloc_failed` flag.
    - `amy.c` `amy_fill_buffer()`: reverb render guard now
      `reverb.level > 0 && stereo_reverb_ready()` (mirrors the echo guard).
    - `amy.h` `reverb_state_t`: new `bool alloc_failed` field.
    - `api.c`: new `bool amy_reverb_alloc_failed(void)` getter exposing the flag so the
      UI can show a no-serial diagnostics indicator.
  - All edit sites are marked inline with `// LOCAL EDIT (2026-06-19): ...`.
  - **Risk:** Low. Behaviour is unchanged when allocation succeeds (normal case). On
    failure, reverb is simply skipped instead of crashing. The added render-path check is
    one pointer comparison per block.
  - **Rollback:** Revert `init_stereo_reverb()` to `void` (drop `free_stereo_reverb()`,
    the rollback branch, and `stereo_reverb_ready()`); restore the original
    `config_reverb()` body; restore the `reverb.level > 0`-only render guard; remove the
    `reverb_state_t.alloc_failed` field and `amy_reverb_alloc_failed()`.

- **Non-AMY-source change for this fix (in `main/main.c`):** the actual OOM. The
  2026-06-18 perf pass pinned `amy_cfg.ram_caps_delay = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`,
  but the FX delay lines don't fit internally (reverb ~108 KB across 10 lines; echo a single
  65536-sample = 256 KB line) while the internal heap's largest free block is ~32 KB.
  Live heap confirmed PSRAM was nearly empty (psram free ≈ 7.7 MB, largest ≈ 7.6 MB), so
  `ram_caps_delay` was moved to `MALLOC_CAP_SPIRAM`. The hot per-block synth allocations
  (`ram_caps_events`/`synth`/`block`/`fbl`) stay internal. The OLED `OOM!` indicator for the
  Reverb menu lives in first-party `components/synth_core/synth_ui.c` (reads
  `amy_reverb_alloc_failed()`), not in AMY source.

## 2026-06-18

- **Performance pass: place the audio render hot path in internal IRAM/SRAM.** Goal was
  lower per-block render time / more CPU headroom (heap use is not a concern). No functional
  behavior change intended. 

  - **`AMY_IRAM_ATTR` macro (`components/amy/src/amy.h`):** new macro = `IRAM_ATTR` on
    `ESP_PLATFORM` (after `#include <esp_attr.h>`), no-op elsewhere. Used instead of an `.lf`
    linker fragment because this build uses GCC LTO, which renames/merges per-function
    `.text.*` sections in the ltrans phase — object/symbol-granularity `noflash` fragment
    rules silently miss and the code stays in flash (verified: symbols stayed at 0x4200…).
    `IRAM_ATTR` rides the symbol's `.iram1` section and survives LTO (verified at 0x4037…).
  - **Functions annotated `AMY_IRAM_ATTR`:**
    - `amy.c`: `combine_controls`, `combine_controls_mult`, `hold_and_modify`, `mix_with_pan`,
      `render_osc_wave`, `amy_render`, `amy_fill_buffer`.
    - `oscillators.c`: `render_lut`, `render_lut_cub`, `render_lut_fm`, `render_lut_fb`,
      `render_lut_fm_fb`, `render_lpf_lut`.
    - `filters.c`: `filter_process`, `dsps_biquad_f32_ansi`, `dsps_biquad_f32_ansi_split_fb`,
      `dsps_biquad_f32_ansi_split_fb_once`, `dsps_biquad_f32_ansi_split_fb_twice`, `scan_max`,
      `parametric_eq_process_top16block`.
    - `delay.c`: `apply_variable_delay`, `apply_fixed_delay`, `stereo_reverb`.
    - `envelope.c`: `compute_mod_value`, `compute_mod_scale`, `compute_breakpoint_scale`.
    - `log2_exp2.c`: `log2_lut`, `exp2_lut`.
    - Several of these (e.g. `filter_process`, `combine_controls*`, `mix_with_pan`, the biquad
      processors, delay walkers) get LTO-inlined into their IRAM callers, so they end up in
      IRAM regardless. `.iram0.text` grew ~57KB → ~71KB; DIRAM ~49.9% used, ~171KB free.
  - **Risk:** Low. IRAM_ATTR only relocates code; semantics unchanged. These functions call
    flash-resident helpers, which is fine while the instruction cache is enabled (normal
    operation — not a cache-disabled / flash-erase context). String literals in the gated
    debug `fprintf` branches stay in flash rodata (only reachable via never-taken `osc==999`
    / debug_flag paths). Coexists with the 2026-06-17 render-lock fix (lock calls are in the
    now-IRAM `amy_render`, calling the flash-resident lock helpers — fine with cache on).
  - **Rollback:** Remove the `AMY_IRAM_ATTR` prefixes from the listed functions and the macro
    + `#include <esp_attr.h>` in `amy.h`.

- **Performance pass: drop `assert()` from pure-DSP hot files (`components/amy/CMakeLists.txt`).**
  The build enables assertions globally (`CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE`),
  which emits runtime checks inside per-sample loops (e.g. `filters.c`
  `assert(FILTER_SCALEUP_BITS == 0)` in the biquad split functions). Appended `-DNDEBUG` via
  `set_property(... APPEND_STRING PROPERTY COMPILE_FLAGS " -DNDEBUG")` to **only**
  `filters.c`, `oscillators.c`, `envelope.c`, `delay.c`, `log2_exp2.c` — files with no
  structural/cross-core state. `amy.c` and the rest of the project keep their asserts
  (osc alloc/free, delta queue, render lock invariants).
  - **Risk:** Low. Only removes always-true asserts from leaf DSP files. Structural safety
    asserts elsewhere are untouched.
  - **Rollback:** Remove the `AMY_DSP_HOT_FILES` block in `components/amy/CMakeLists.txt`.

- **Considered but NOT changed (recorded so we don't re-investigate):**
  - LUT wavetable data placement in DRAM — `PROGMEM` (the tag) is shared with the 102KB
    `pcm` table + piano data, so a blanket redefine is unsafe; per-table `DRAM_ATTR` is
    broad/fragile; and with flash QIO + 32KB I/D-cache the ≤4KB LUTs cache well. Skipped.
    (Revisited 2026-09-18: the saw set is now pinned per table, see "Active local edits".)
  - Active-oscillator index in `amy_render` — would require mutating an active set from the
    zero-amp reaper *inside* the render loop (amy.c:1544), the same mid-iteration mutation
    behind the 2026-06-17 LoadProhibited race. After the IRAM + internal-SRAM moves each
    scan check is only a few cycles, so high risk / low gain. Rejected.
  - `-ffast-math` — AMY uses NaN sentinels (`nanf` / `AMY_IS_SET`); fast-math assumes no
    NaNs and would break those checks. Rejected.

  (Non-AMY-source changes for this pass live in `main/main.c` (ram_caps → internal SRAM) and
  `sdkconfig.defaults` (I-cache 16→32KB, flash QOUT→QIO).)

## 2026-06-17

- **Bug fix (SMP crash): unlocked render path races patch-toggle frees in `components/amy/src/amy.c`**
  - **Symptom:** Intermittent `Guru Meditation Error: Core 1 panic'ed (LoadProhibited)` while toggling patches from the sequencer UI. Backtrace: `combine_controls_mult` (amy.c:1340) → `hold_and_modify` (amy.c:1406) → `render_osc_wave` (amy.c:1486) → `amy_render` (amy.c:1563) → `amy_render_audio` (i2s.c) → `amy_update` → `amy_usb_render_task`. `EXCVADDR=0x00000008` = NULL `synth[osc]` base plus a struct field offset.
  - **Root cause:** `amy_render`/`render_osc_wave`/`hold_and_modify` walk `synth[]`/`msynth[]` holding **no lock**, while the delta path (`play_delta` → `ensure_osc_allocd` → `free_osc()`+`alloc_osc()`, plus reset/patch-load deltas) takes `amy_queue_lock` and `free()`s/reallocates those same structs. In this build (`multicore=0`, `multithread=0`) deltas are drained on Core 1 right before render, so the racing actor is **Core 0**: the sequencer `esp_timer` tick and UI calling `add_delta_to_queue()` and other osc alloc/free paths. The `synth[osc] != NULL` check in `amy_render` (line 1561) is a TOCTOU — `synth[osc]` can be freed between the check and the deref inside `hold_and_modify`.
  - **Fix (option 1):** Wrap the entire `amy_render` body in `amy_grab_lock()` / `amy_release_lock()` (the existing `amy_queue_lock` semaphore), so structural mutations cannot run mid-render. Verified deadlock-safe: no code reachable from the render path calls `add_delta_to_queue()` or `amy_grab_lock()` (would deadlock the non-recursive mutex), and with `multicore=0` there is no cross-core notify-while-holding-lock path. (If multicore is ever enabled, each `amy_render` invocation takes/releases the lock around its own work; the inter-core notify/wait happens in the caller, outside the locked region.)
  - **Risk:** Low–moderate. The lock is now held for a full render block (~hundreds of µs). The Core 0 sequencer enqueue (`add_delta_to_queue`) briefly blocks on it, but enqueue is a short list insert. No new allocation in the hot path.
  - **Rollback:** Remove the `amy_grab_lock()` after `AMY_PROFILE_START(AMY_RENDER)` and the matching `amy_release_lock()` before `AMY_PROFILE_STOP(AMY_RENDER)` in `amy_render`.

- **Bug fix (external AMY source change): missing `chained_osc` NULL guard in `render_osc_wave`, `components/amy/src/amy.c`**
  - **Root cause:** Upstream AMY dereferences `synth[chained_osc]->status` (the chained-osc recursion in `render_osc_wave`, ~line 1521) with **no NULL check**. A `chained_osc` can reference a slot that was freed or never allocated during a patch toggle, faulting the same way as above. This is a strict upstream bug, independent of the locking fix.
  - **Fix:** Added `synth[chained_osc] != NULL &&` to the existing `synth[chained_osc]->status == SYNTH_AUDIBLE` condition. Marked inline as a LOCAL EDIT.
  - **Risk:** Negligible. Adds one pointer comparison; when the chained slot is NULL the chained osc is simply skipped (correct — it cannot be audible).
  - **Rollback:** Remove the `synth[chained_osc] != NULL &&` clause.

## 2026-03-21

- Added ESP-IDF 6.0 compatibility comments and updated FreeRTOS task entry points in `components/amy/src/amy_midi.c` and `components/amy/src/i2s.c` so they use the required `void *` task signature.
- This is an external dependency compatibility edit, not a first-party AMY refactor.
- Excluded `components/amy/src/usb.c` from the ESP-IDF component build because this project provides its own USB implementation and does not need the MicroPython/Arduino USB path.

## 2026-03-31

- **Bug fix:** `AMY_SAMPLE_RATE` on `ESP_PLATFORM` defaulted to `44100` (the `#else` branch in `amy.h` lines 57–65), but `CONFIG_UAC_SAMPLE_RATE=48000` in `sdkconfig` means the TinyUSB UAC descriptor advertises 48 kHz to the Windows host. This caused AMY to render at 44.1 kHz while the host consumed samples as if they were 48 kHz — audio played ~88 cents flat and ~8% slow.
  - Added `#elif defined ESP_PLATFORM` → `48000` between the `__EMSCRIPTEN__` and `#else` cases in `amy.h`.
  - **Motivation:** UAC sample rate and AMY render rate must match; the minimal fix is a single added clause in the platform SR block.
  - **Risk:** Low. 48 kHz on S3 is well within hardware capability. PCM samples are internally stored at 22050 Hz and resampled — no change needed there. No time-sensitive path is altered; block size (256) stays the same.
  - **Rollback:** Remove the `#elif defined ESP_PLATFORM` / `48000` clause.

- **Config fix (not an AMY patch):** `platform.multithread` and `platform.multicore` must be set to `0` in `main.c` when using `AMY_AUDIO_IS_NONE`. With the defaults (`1`/`1`), `amy_platform_init()` spawns FABT and captures `app_main`'s task handle as `amy_update_handle`, causing a permanent deadlock between FABT and our `amy_usb_render_task` — `render_blocks` and `seq_tick` stay at 0. No change to AMY source required; fixed in `main/main.c`.

## 2026-04-02

- **Bug fix:** Guarded `esp_poll_midi()` in `components/amy/src/i2s.c` so the ESP-IDF update path only touches UART MIDI when `AMY_MIDI_IS_UART` is enabled.
  - **Root cause:** `amy_default_config()` sets `c.midi = AMY_MIDI_IS_NONE` for ESP32 builds, but `amy_update_tasks()` still called `esp_poll_midi()` whenever `platform.multithread == 0`. That reached `uart_read_bytes()` on UART1 without a driver installed and produced `uart driver error` logs.
  - **Motivation:** Prevent spurious UART errors when this project runs AMY in USB-audio-only mode.
  - **Risk:** Low. The change is a narrow runtime guard around the existing MIDI poll path and does not affect builds that actually enable UART MIDI.
  - **Rollback:** Remove the `AMY_MIDI_IS_UART` condition and restore the unconditional poll.

## 2026-04-03

- **Bug fix (SMP crash): `sequences[]` race in `components/amy/src/sequencer.c`**
  - **Root cause:** On ESP32-S3 SMP, `sequencer_process_tick()` (called from AMY's `esp_timer` callback task) reads and walks `sequences[tag].deltas` without holding any lock, while `sequencer_add_event()` (called from `button_handler_task` via `amy_add_event()`) calls `delta_release_list(sequences[tag].deltas)` + rebuilds the chain, also without a lock. Concurrent execution on two cores causes a use-after-free: the timer task dereferences a delta node that the button task has already returned to the free pool and zeroed, producing `LoadProhibited` at `EXCVADDR=0x000002f0`.
  - `amy_queue_lock` was not usable here because `add_delta_to_queue()` is called *inside* `sequencer_process_tick()` and also grabs `amy_queue_lock` — wrapping the outer function would deadlock a non-recursive mutex.
  - **Fix:** Added `SEQ_LOCK` / `SEQ_UNLOCK` macros backed by a new `static SemaphoreHandle_t s_seq_lock` (ESP), `pthread_mutex_t` (POSIX), or no-op (bare-metal). Lock is created in `sequencer_init()` before `_sequencer_start()`. Held across the entire `sequences[tag]` mutation in `sequencer_add_event()` and across the full for-loop in `sequencer_process_tick()`. `add_delta_to_queue()` continues to independently grab `amy_queue_lock` (no nesting conflict).
  - **Risk:** Low. The mutex is a short critical section (one tag slot per `add_event` call, <1 ms per tick loop). No new allocation in hot path. Timer callback is at 500 µs cadence; mutex contention adds negligible latency.
  - **Rollback:** Remove the `SEQ_LOCK()`/`SEQ_UNLOCK()` calls and the lock variable block at the top of `sequencer.c`.

- **Bug fix (silent melodic layer): `max_sequencer_tags` too small — in `main/main.c`** *(not an AMY source patch)*
  - **Root cause:** `amy_default_config()` sets `max_sequencer_tags = 256`. Our tag formula assigns melodic layer (index 1) tags starting at `1 × (4×32×2) = 256`. `sequencer_add_event()` checks `tag > max_sequences` (strictly greater), so tag 256 slips through and writes to `sequences[256]` — one past the end of the 256-element array (UB/memory corruption). All tags >256 are silently dropped. Result: every melodic note event is either corrupted or discarded; no audio.
  - **Fix:** Added `amy_cfg.max_sequencer_tags = 1100` in `main.c` before `amy_start()`. Our highest tag is 1039 (preview slot for last layer/track), so 1100 gives safe headroom above the off-by-one in AMY's bound check.
  - **Rollback:** Remove the `amy_cfg.max_sequencer_tags = 1100` line (reverts to default 256).

## 2026-03-22

- **Bug fix:** `AMY_RENDER_TASK_PRIORITY` and `AMY_FILL_BUFFER_TASK_PRIORITY` in `components/amy/src/amy.h` changed from `ESP_TASK_PRIO_MAX` to `ESP_TASK_PRIO_MAX - 1`.
  - `ESP_TASK_PRIO_MAX` equals `configMAX_PRIORITIES` (25). FreeRTOS asserts `uxPriority < configMAX_PRIORITIES`, so passing 25 is unconditionally invalid and causes an immediate boot crash.
  - Filed upstream as [#625](https://github.com/shorepine/amy/issues/625).
  - **Rollback:** revert the `- 1` subtraction in both defines.