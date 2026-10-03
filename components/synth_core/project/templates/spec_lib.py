"""Building blocks for project specs (the template modules in this directory).

Firmware enums, voice-param builders and step-pattern helpers, all producing
amp_codec's dict model. A spec composes these into build() -> project dict;
amp_codec.encode_file() turns that into a Pnn.amp image. The firmware
constants copied here are checked against their headers by gen_templates.py.
"""
import copy
import amp_codec as C

# ── firmware / AMY enums ───────────────────────────────────────────────────
# Pitch classes
C_, Db, D, Eb, E, F, Gb, G, Ab, A, Bb, B = range(12)

# chord_type_t (quantizer.c s_chord_intervals order)
CHORD_MAJ, CHORD_MIN, CHORD_MAJ7, CHORD_MIN7, CHORD_DOM7, CHORD_SUS2, CHORD_SUS4 = range(7)
CHORD_DIM, CHORD_AUG, CHORD_MIN9, CHORD_MAJ9, CHORD_MAJ6, CHORD_MIN6, CHORD_DOM9 = range(7, 14)

# quantizer.c s_scales[] index
SCALE_CHROMATIC, SCALE_MAJOR, SCALE_MINOR, SCALE_DORIAN, SCALE_PHRYGIAN = range(5)
SCALE_LYDIAN, SCALE_MIXOLYDIAN, SCALE_MIN_PENT, SCALE_MAJ_PENT, SCALE_HARM_MINOR = range(5, 10)

FILTER_LPF, FILTER_BPF, FILTER_HPF, FILTER_LPF24 = 1, 2, 3, 4
ENV_NORMAL, ENV_LINEAR, ENV_DX7, ENV_TRUE_EXP = 0, 1, 2, 3
EGT_PITCH, EGT_CUTOFF, EGT_DRIVE, EGT_MIX = 0, 1, 2, 3
LFO_SINE, LFO_TRI, LFO_SAW_UP, LFO_SAW_DOWN, LFO_SQUARE, LFO_RANDOM = range(6)
LFO_TGT_FILTER, LFO_TGT_AMP, LFO_TGT_PITCH, LFO_TGT_PAN, LFO_TGT_SCAN = 1, 2, 4, 8, 16
LFO_1_8, LFO_1_4, LFO_1_2, LFO_1BAR, LFO_2BAR, LFO_4BAR, LFO_1_16 = range(7)
DIST_CLIP, DIST_FOLD, DIST_CRUSH = 1, 2, 4
SEQ_LAYER_DRUM, SEQ_LAYER_MELODIC = 0, 1
SCOPE_LAYER, SCOPE_TRACK = 0, 1
FOLLOW_CHORD, FOLLOW_ROOT, FOLLOW_OFF = 0, 1, 2
ARP_RATE_1_1, ARP_RATE_1_4, ARP_RATE_1_8, ARP_RATE_1_16, ARP_RATE_1_32 = range(5)
ARP_UP, ARP_DOWN, ARP_SLOT = 0, 1, 2
ARP_QUANT_OWN, ARP_QUANT_GLOBAL, ARP_QUANT_CHORD = 0, 1, 2
ARP_REST = -2
ARP_EMPTY = -1
DRONE_SRC_WAVE, DRONE_SRC_PATCH = 0, 1
DRONE_RATE_1_4, DRONE_RATE_1_8, DRONE_RATE_1_16, DRONE_RATE_1_32, DRONE_RATE_1_1 = range(5)
DRONE_PAT_FULL, DRONE_PAT_FOUR, DRONE_PAT_OFFBEAT, DRONE_PAT_GALLOP, DRONE_PAT_DUB = range(5)
DRONE_FOLLOW_OFF, DRONE_FOLLOW_ROOT, DRONE_FOLLOW_CHORD = 0, 1, 2
WAVE_SINE, WAVE_PULSE, WAVE_SAW_DOWN, WAVE_SAW_UP, WAVE_TRIANGLE = range(5)   # AMY wave, drone WAVE source

# fx_group_t bits in GLOB split_flags: that group renders on its own bus
SPLIT_DRUMS, SPLIT_DRONES, SPLIT_CLIPS = 0x02, 0x04, 0x08

# Patches (sequencer_core.h numbering)
P_SINE, P_SAW_DOWN, P_SAW_UP, P_PULSE, P_TRIANGLE, P_NOISE, P_KS = range(257, 264)
P_BASS_SUB_DETUNE, P_BASS_ACID, P_BASS_DX7 = 264, 265, 266
P_WT_PPG = 269

# PCM drum presets. 808 = the ROM bank (0..18); the rest are the gamma9001
# banks in the 'drums' partition (256..391, pcm_gamma9001.h order).
PCM_808_BD1, PCM_808_BD2, PCM_808_BD3, PCM_808_CLAP, PCM_808_CLAVE = 0, 1, 2, 3, 4
PCM_808_CONGA_HI, PCM_808_CONGA_LO, PCM_808_CONGA_MID, PCM_808_COWBELL = 5, 6, 7, 8
PCM_808_HH, PCM_808_OH, PCM_808_SHAKER, PCM_808_SD1, PCM_808_SD2, PCM_808_SD3 = 9, 10, 11, 12, 13, 14
PCM_808_RIM, PCM_808_TOM_LO, PCM_808_TOM_HI, PCM_808_CYMBAL = 15, 16, 17, 18
PCM_909_BD, PCM_909_BD_LO, PCM_909_CLAP, PCM_909_CRASH, PCM_909_HH = 256, 257, 258, 259, 260
PCM_909_HH_LONG, PCM_909_HH_SHORT, PCM_909_OH, PCM_909_OH_LONG, PCM_909_OH_SHORT = 261, 262, 263, 264, 265
PCM_909_RIDE, PCM_909_RIM, PCM_909_SD, PCM_909_SD_SHORT = 266, 267, 268, 269
PCM_LINN_BD, PCM_LINN_COWBELL, PCM_LINN_CYMBAL, PCM_LINN_HH, PCM_LINN_OH = 273, 275, 276, 277, 278
PCM_LINN_RIM, PCM_LINN_SD, PCM_LINN_TAMB, PCM_LINN_TOM = 279, 280, 281, 282
PCM_POWER_KICK, PCM_POWER_SIDESTICK, PCM_POWER_SNARE, PCM_POWER_CLAP = 311, 313, 314, 315
PCM_POWER_GATED_SNARE, PCM_POWER_HH, PCM_POWER_OH, PCM_POWER_RIDE = 316, 318, 322, 327
PCM_SHAKER = 352                # percussion bank "Simple Shaker"
PCM_TAMB = 361                  # percussion bank "Tamb 001"

# Drum-synth fallbacks per row, idle while the PCM engine plays.
DRUM_SYNTH_PATCHES = [58, 245, 221, 220, 220]

SEQ_CHORD_BASE = 200
GATE_HOLD = 0xFFFF              # layer gate_pct: hold each note until the row's next trig


def chord_note(slot):
    return SEQ_CHORD_BASE + slot


# ── voice-param builders ───────────────────────────────────────────────────

def env(a, d, s, r, t=ENV_NORMAL):
    return dict(attack_ms=a, decay_ms=d, sustain_pct=s, release_ms=r, eg_type=t)


# What sequencer_core_add_layer seeds into a melodic row (Kconfig defaults of
# this build); written into idle rows so a resave matches.
MEL_ENV_SEED = env(5, 250, 30, 200)
MEL_ENV1_SEED = env(15, 450, 25, 400)


def filt(ftype, cutoff, res, eg1_cutoff=0.0, eg1_pitch=0.0):
    f = C.default_filter()
    f.update(filter_type=ftype, cutoff_hz=float(cutoff), resonance=float(res), enabled=True)
    f['eg_depth'][1][EGT_CUTOFF] = float(eg1_cutoff)
    f['eg_depth'][1][EGT_PITCH] = float(eg1_pitch)
    return f


def lfo(wave, rate, targets, depth=50, flt_oct_q=0, mode=0):
    l = C.default_lfo()
    l.update(enabled=True, mode=mode, wave=wave, rate=rate, depth=depth, targets=targets,
             flt_oct_q=flt_oct_q)
    return l


def dist(kind, drive, mix, bits=8, rate=8):
    return dict(type=kind, drive=drive, bits=bits, rate=rate, mix=mix)


def vp(amp_env=None, eg1=None, flt=None, mod=None, dst=None, trim=1.0, melodic=True):
    v = C.default_vp()
    if melodic:
        v['env'] = copy.deepcopy(MEL_ENV_SEED); v['env1'] = copy.deepcopy(MEL_ENV1_SEED)
    if amp_env: v['env'] = amp_env; v['env_authored'] = True
    if eg1: v['env1'] = eg1; v['env1_authored'] = True
    if flt: v['filter'] = flt; v['filter_authored'] = True
    if mod: v['lfo'] = mod; v['lfo_authored'] = True
    if dst: v['dist'] = dst; v['dist_authored'] = True
    v['amp_trim'] = trim
    return v


# ── step patterns ──────────────────────────────────────────────────────────

def put_steps(L, track, steps, base_note):
    """steps: {step: dict(ofs, vel, prob, every, ratchet, taper, nudge, prev)}"""
    for s in range(C.SEQ_MAX_STEPS):
        L['step_note'][track][s] = base_note
    for s, st in steps.items():
        L['grid'][track][s] = 1
        L['step_pitch_ofs'][track][s] = st.get('ofs', 0)
        L['step_velocity_adj'][track][s] = st.get('vel', 0)
        L['step_prob'][track][s] = st.get('prob', 100)
        L['step_every'][track][s] = st.get('every', 1)
        L['step_ratchet'][track][s] = st.get('ratchet', 1)
        L['step_ratchet_taper'][track][s] = st.get('taper', 0)
        L['step_nudge'][track][s] = st.get('nudge', 0)
        L['step_prev'][track][s] = 1 if st.get('prev') else 0


def hits(pattern, **common):
    """'x...x...' -> {step: {...}}; X = accent (+20 velocity)."""
    out = {}
    for i, ch in enumerate(pattern.replace(' ', '')):
        if ch in 'xX':
            d = dict(common)
            if ch == 'X':
                d['vel'] = d.get('vel', 0) + 20
            out[i] = d
    return out


def merge(base, over):
    out = copy.deepcopy(base)
    for s, d in over.items():
        if d is None:
            out.pop(s, None)
        else:
            out.setdefault(s, {}).update(d)
    return out


def line(pattern, offsets, **common):
    """hits() with per-hit pitch offsets, in hit order: line('x.x.', [0, 12])."""
    out = hits(pattern, **common)
    assert len(out) == len(offsets), (pattern, offsets)
    for s, o in zip(sorted(out), offsets):
        out[s]['ofs'] = o
    return out


# ── layers ─────────────────────────────────────────────────────────────────

def melodic_layer(swing, gate, groove, voices, porta=0, chord=None):
    """chord=(root_pc, chord_type) turns chord mode on: chord-preset rows
    follow the progression by rigid transpose, plain rows are voiced together
    as the live chord. Without it, rows snap to the global scale quantizer."""
    L = C.default_layer(SEQ_LAYER_MELODIC)
    L.update(num_steps=16, num_tracks=4, patch_scope=SCOPE_TRACK, num_voices=voices,
             gate_pct=gate, groove_pct=groove, swing_pct=swing, portamento_ms=porta)
    if chord:
        L.update(chord_mode=True, chord_root=chord[0], chord_type=chord[1])
    for t in range(C.SEQ_TRACKS):                  # idle rows until a part claims one
        L['tracks'][t].update(base_note=57 + 3 * t, patch=P_SINE, vp=vp(trim=1.0))
        put_steps(L, t, {}, 57 + 3 * t)
    return L


def claim(L, t, patch, slot_or_note, v, steps, **track):
    """Put a part on row t: slot_or_note < 8 is a chord-preset slot, else a MIDI note."""
    note = chord_note(slot_or_note) if slot_or_note < 8 else slot_or_note
    L['tracks'][t].update(base_note=note, patch=patch, vp=v, **track)
    put_steps(L, t, steps, note)


def chord_rows(L, patch, refs, v, steps):
    """One chord across plain rows, row t anchored at refs[t]. On a chord-mode
    layer the rows are voiced together as the live progression chord (root,
    third, fifth, seventh spread over the rows), so the shape follows chord
    quality too - unlike a chord-preset row, which transposes rigidly.
    Every live row of the layer takes part in the voicing, so refs covers
    them all (num_tracks is at least 4)."""
    assert len(refs) == L['num_tracks']
    for t, n in enumerate(refs):
        claim(L, t, patch, n, copy.deepcopy(v), steps)


def drum_layer(rows, swing=0, groove=70):
    """rows: [(pcm_preset, note, steps, vp or None)], 4 or 5 of them.
    A None vp leaves the row unauthored, so the firmware seeds its bank's
    defaults (the 808 bank tunes row 0 as the kick and row 2 as the hat)."""
    assert 4 <= len(rows) <= C.SEQ_TRACKS
    L = C.default_layer(SEQ_LAYER_DRUM)
    L.update(num_steps=16, num_tracks=len(rows), swing_pct=swing, groove_pct=groove, num_voices=1)
    for t in range(C.SEQ_TRACKS):
        L['tracks'][t].update(patch=DRUM_SYNTH_PATCHES[t])
    for t, (preset, note, steps, v) in enumerate(rows):
        L['tracks'][t].update(base_note=note, pcm_preset=preset,
                              vp=v if v else vp(melodic=False))
        put_steps(L, t, steps, note)
    L['patch'] = L['tracks'][0]['patch']
    return L


# ── global sections ────────────────────────────────────────────────────────

def glob(bpm, key_root, scale, bus0=None, bus1=None, split=SPLIT_DRUMS | SPLIT_CLIPS,
         master=1.5):
    """key_root: pitch class; the quantizer root sits in octave 3 (A = 57)."""
    fx = [C.default_fx() for _ in range(C.FX_BUS_COUNT)]
    fx[0].update(bus0 or {})
    fx[1].update(bus1 or {})
    return dict(bpm=bpm, master_volume=master, quant_enabled=True, quant_root=48 + key_root,
                quant_scale=scale, drum_engine=1, presets_alter_global=False,
                split_flags=split, fx=fx)


def arp_off():
    return arp(False, P_PULSE, ARP_UP, ARP_RATE_1_16, [ARP_EMPTY] * C.ARP_MAX_SLOTS)


def arp(enabled, patch, direction, rate, slots, octaves=1, gate=50, quant=ARP_QUANT_GLOBAL,
        amp_env=None, flt=None, mod=None, root=57, scale=SCALE_MINOR, amp=1.0, porta=0):
    slots = list(slots) + [ARP_EMPTY] * (C.ARP_MAX_SLOTS - len(slots))
    return dict(enabled=enabled, patch=patch, dir=direction, octaves=octaves, rate=rate,
                gate_pct=gate, scale=scale, root=root, portamento_ms=porta, amp_scale=amp,
                slots=slots, env=amp_env or env(2, 160, 0, 120), env2=env(5, 300, 20, 300),
                filter=flt or C.default_filter(), lfo=mod or C.default_lfo(),
                quant_mode=quant, dist=C.default_dist())


def drone_off():
    return drone(False, CHORD_SUS4, 57)


def drone(enabled, chord, root, wave=WAVE_SAW_DOWN, peak=0.6, duck=0.0, rate=DRONE_RATE_1_4,
          sweep=(400.0, 1600.0), sweep_bars=8, res=1.2, pattern=DRONE_PAT_FULL,
          amp_env=None, sub=False, follow=DRONE_FOLLOW_OFF):
    return dict(enabled=enabled, source=DRONE_SRC_WAVE, wave=wave, chord=chord, root=root,
                patch=25, resonance=res, amp_peak=peak, amp_duck=duck, amp_trim=1.0,
                rate=rate, sub_enabled=sub, sub_interval=-12, sweep_lo=float(sweep[0]),
                sweep_hi=float(sweep[1]), sweep_bars=sweep_bars, gate_len=0.5, swing=0,
                blip=0.0, pattern=pattern, follow=follow, env=amp_env or env(200, 300, 100, 600),
                env2=env(15, 400, 25, 400))


def project(glob_, layers, arp_=None, drone_=None, progression=None, chords=None):
    chords = chords or {}
    return dict(glob=glob_, layers=layers, arp=arp_ or arp_off(), drone=drone_ or drone_off(),
                prog=dict(enabled=bool(progression), entries=progression or []),
                chords=[chords.get(i, []) for i in range(C.SEQ_CHORD_SLOTS)],
                clip=dict(bars=4, stereo=False, tail=2, level=[100, 100], mode=[0, 0], after=0),
                pgen=dict(style=0, len=4, bars=2, ext=0, var=0, seed=0))


def apply_mutes(p, parts, mutes):
    """Silence parts for stem renders. parts: names for p['layers'] in order,
    then optionally 'arp'/'drone'; mutes: part names or 'layer:row'."""
    for n, L in zip(parts, p['layers']):
        for t, T in enumerate(L['tracks']):
            if n in mutes or '%s:%d' % (n, t) in mutes:
                T['mute'] = True
    if 'arp' in mutes: p['arp']['enabled'] = False
    if 'drone' in mutes: p['drone']['enabled'] = False
    return p
