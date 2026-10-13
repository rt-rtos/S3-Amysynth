 Runs on a timer at ~20 Hz (every 50 ms), from a normal low-priority
 task -- NOT inside the audio render callback.

 One instance of this state per LFO'd synth:
   phase   : float 0..1     (free-running, persists between ticks)
   held    : float -1..1    (sample-and-hold value, RANDOM wave only)
 Config per synth: wave, rate_hz, depth (0..1), targets, and the
 known base filter cutoff.

every 50 ms, for each synth with an LFO enabled:

1. advance the oscillator by hand

```
    phase += rate_hz * 0.050
    if phase >= 1.0:
        phase -= 1.0
        if wave == RANDOM: held = new_random(-1..1)
```
2. evaluate the waveform at this phase -> value in -1..+1
   
```
    value = sine / triangle / saw_up / saw_down / square (phase)
            or `held` for RANDOM

    d = depth   # 0..1
```

3. inject: one amy.send() per target per tick, rewriting the CONSTANT term
        of each modulated parameter's ControlCoefficients.
        (Only the named coefficient changes; note/vel/env/mod
        coefficients the patch set up are left alone.)
```
    amy.send(synth=s,
        filter_freq = base_cutoff * 2**(voice_lfo_filter_octaves(lfo) * value),
                                                         # Flt octave range, 0.25..4 oct
        pan         = 0.5 + 0.5 * d * value)             # around center

    amy.send(synth=s, osc=amp_osc,                       # amp: osc 0 on ALGO voices
        amp         = 1.0 - d * (0.5 - 0.5 * value))     # dips 1.0 -> 1.0-d, never boosts
                                                         # amp_osc: sequencer_core_patch_amp_osc()

    amy.send(synth=s, osc=0,                             # pitch: osc 0 only
        freq        = SEQ_LFO_PITCH_BASE_HZ * 2**(d * VOICE_LFO_DEPTH_PITCH * value))
                                                         # 440 Hz anchor; depth 1.0 = +/-1 semitone
```
*(only the parameters the user actually targeted are included; the depth constants are `VOICE_LFO_DEPTH_*` in `voice_config.h`, the loop is `sequencer_core_lfo_service()` in `seq_core_editors.c`. The rate is `lfo_rate_to_hz()` capped at `SEQ_LFO_SW_MAX_HZ`, so the fast end still gets at least four steps per cycle.)*

> When this stepper runs: with `CONFIG_SEQ_LFO_SHARED` (the default) an LFO normally lives in a shared LFO synth - a two-osc AMY synth of its own that the voice names in mod slot 1, so a patch's own oscs are never borrowed as a modulator (`lfo_bank.h`; ENGINE-SEMANTICS.md, "LFO: native carrier vs software stepper"). This stepper is the fallback for a voice that has none: the bank is full, or the patch family has no target map (the piano, additive voices). Without `CONFIG_SEQ_LFO_SHARED` it serves every patch string, since a mod_source needs a free oscillator and a loaded patch voice (Juno, DX7) has none - naming one of its oscs mutes it. The modulation is done from outside: a control-rate task recomputes each target parameter's constant term and re-sends it as a normal parameter update, exactly as if a very fast hand were turning the knob 20 times a second. The patch's internal structure is untouched; the trade-offs are 50 ms zipper-stepping (audible on square/random-to-amp without slew) and that the patch's original constants on the modulated parameters are overwritten rather than restored when the LFO turns off. The stepper stands down, restoring the targets it drove, as soon as the voice gets a shared LFO synth.

## DIST targets

`LFO_TARGET_DIST_DRIVE` and `LFO_TARGET_DIST_MIX` are two independent target
bits sweeping the distortion stage's drive and mix around the row's committed
`seq_dist_t` - check either, or both. They are the last two rows of the LFO
editor's target checklist (it scrolls; the panel fits five rows), which is why
there is no separate reach field.

A voice on a shared LFO synth drives both through AMY's COEF_MOD1 rails
(without `CONFIG_SEQ_LFO_SHARED`, a native carrier patch through its COEF_MOD
rails; ENGINE-SEMANTICS.md, "LFO: native carrier vs software stepper"). This
stepper serves the rest: each tick rewrites the drive and mix coefs' CONST
term. Law and clamps in `voice_push_dist_lfo()` (voice_config.c), matching the
rail law exactly.

Either way the shaper being OFF makes the target inert - the LFO never switches
distortion on (the AMY combine is gated on `synthinfo.dist_stages`; the stepper checks
`base->type`). Restore on disable is the committed dist block via
`voice_apply_dist()`: there is no context-free neutral, so
`lfo_push_target_neutral()` skips DIST (same shape as FILTER). Disabling a
shared LFO synth's voice also clears the dist COEF_MOD1 rails (`voice_lfo_wire()`
unwires every rail); without `CONFIG_SEQ_LFO_SHARED` the topo's disabled branch
clears the COEF_MOD ones.
