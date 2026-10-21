"""Neon Rain: a finished track, not a genre starting point. Dark cyberpunk
synthwave at 84 BPM in F phrygian.

Twelve bars on Fm7, then Dbmaj7 (4), Bbm7 (2) and Gbmaj7 (2): a 20-bar
cycle. The low end is the stutter drone: a saw on the chord root with a sine
sub an octave down, a continuous pedal that swells on the offbeat 16ths
between the quarter-note kicks (18 dB duck between them), on its own dry
bus. A 808 kick tuned to F1, a gated snare on 2 and 4, 16th
hats around the downbeats, a shaker and an open hat every fourth bar keep the
kit restrained (dry, on the drum bus). On bus 0, under a wide chorus, a dark
reverb, a dotted-16th echo, a low-mid EQ lift and a light sample-rate crush:
- a custom-wavetable arp in 16ths, F Ab C over a low F, two octaves in slot
  order, held on F phrygian so the same notes sit over every chord;
- detuned saw pads voiced over the progression, pumping on the quarter note
  (amp LFO) and opening over 16 bars;
- band-passed noise rain drifting in pitch and pan;
- a scale-locked bell motif two bars in every four;
- a noise downlifter every eight bars.

Arp and pad filters breathe on 16-bar LFOs against the 20-bar harmony, so the
combination repeats only every 80 bars. Each part stands alone for live
muting: drums + drone is the groove, pads + arp the breakdown.

The pads sit on a 16-step layer so they retrigger on every chord change; the
32-step layers (drums, motif) carry nothing that must line up with the chords,
because a 32-step layer's phase against the progression depends on when Play
lands. `every` counts layer loops from Play, so the motif and downlifter
phase is not tied to the progression either.
"""
from spec_lib import *

NAME = 'NEON RAIN'
BPM = 84
KEY, SCALE = F, SCALE_PHRYGIAN
PROGRESSION = [(F, CHORD_MIN7, 4), (F, CHORD_MIN7, 4), (F, CHORD_MIN7, 4), (Db, CHORD_MAJ7, 4),
               (Bb, CHORD_MIN7, 2), (Gb, CHORD_MAJ7, 2)]


def steps32(fn):
    """{step: dict} for each step s of a 32-step row where fn(s) is not None."""
    return {s: d for s in range(32) for d in (fn(s),) if d is not None}


KICK = merge(steps32(lambda s: {} if s % 4 == 0 else None), {27: dict(prob=35, vel=-25)})
SNARE = merge(steps32(lambda s: {} if s % 8 == 4 else None), {31: dict(prob=25, vel=-40)})
HAT = steps32(lambda s: None if s % 4 == 0 else
              dict(vel=-5) if s % 4 == 2 else dict(vel=-28, prob=85))
OPEN = {14: dict(every=2, vel=-12), 30: dict(prob=50, vel=-18)}
SHAKE = steps32(lambda s: dict(vel=-10, prob=90) if s % 2 else dict(vel=-25, prob=60))

MOTIF = {s: dict(ofs=o, every=2) for s, o in
         ((0, 7), (3, 3), (6, 0), (10, -2), (16, 7), (19, 8), (22, 7), (26, 3), (29, 0))}
P_JUNO_PLUCK_BELL = 52


def drums():
    L = drum_layer([
        (PCM_808_BD1,           57, KICK,  vp(amp_env=env(0, 380, 0, 120), trim=0.35,
                                              melodic=False)),        # tail on F1
        (PCM_POWER_GATED_SNARE, 58, SNARE, vp(trim=0.8, melodic=False)),
        (PCM_909_HH_SHORT,      62, HAT,   vp(flt=filt(FILTER_HPF, 6000, 0.7), trim=0.8,
                                              melodic=False)),
        (PCM_909_OH_SHORT,      60, OPEN,  vp(flt=filt(FILTER_HPF, 4000, 0.7), trim=0.9,
                                              melodic=False)),
        (PCM_SHAKER,            60, SHAKE, vp(flt=filt(FILTER_HPF, 5000, 0.7), trim=0.65,
                                              melodic=False)),
    ], groove=40)
    L['num_steps'] = 32
    return L


def pulse():
    L = melodic_layer(0, gate=55, groove=30, voices=1, chord=PROGRESSION[0][:2])
    L.update(num_steps=32, patch_scope=SCOPE_TRACK)
    # Scale-locked rather than ROOT follow: a rigid transpose would put A, E
    # and B over Db and Gb, while every F phrygian note of the line sits on
    # all four chords. The patch sounds an octave above the row note.
    claim(L, 0, P_JUNO_PLUCK_BELL, 53,
          vp(amp_env=env(3, 900, 0, 700), flt=filt(FILTER_LPF, 2600, 0.8), trim=0.8),
          MOTIF, follow=FOLLOW_OFF)
    # Downlifter. The gate ends the note within the step, so the release
    # carries the downward band sweep.
    claim(L, 1, P_NOISE, 72,
          vp(amp_env=env(5, 2600, 0, 2600), eg1=env(5, 2600, 0, 2600),
             flt=filt(FILTER_BPF, 600, 2.0, eg1_cutoff=3.0), trim=0.4),
          {0: dict(every=4)}, follow=FOLLOW_OFF)
    L['patch'] = P_JUNO_PLUCK_BELL
    return L


def pads():
    L = melodic_layer(0, gate=GATE_HOLD, groove=0, voices=1, chord=PROGRESSION[0][:2])
    L['patch_scope'] = SCOPE_TRACK
    v = vp(amp_env=env(900, 0, 100, 1800), flt=filt(FILTER_LPF24, 1800, 0.9),
           mod=lfo(LFO_SAW_UP, NOTE_DIV_1_4, LFO_TGT_AMP, depth=30), trim=0.32)
    v['lfo2'] = lfo(LFO_SINE, NOTE_DIV_16BAR, LFO_TGT_FILTER, depth=60, flt_oct_q=4)
    v['lfo2_authored'] = True
    chord_rows(L, P_SAW_DOWN, [65, 68, 72, 75], v, {0: {}})   # F4 Ab4 C5 Eb5 anchors
    for T in L['tracks'][:4]:
        T.update(uni_count=4, uni_detune=14, uni_spread=100, uni_blend=80)
    # Rain on the fifth row: FOLLOW_OFF keeps it out of the chord voicing.
    rain = vp(amp_env=env(400, 0, 100, 1500), flt=filt(FILTER_BPF, 5000, 1.2),
              mod=lfo(LFO_SINE, NOTE_DIV_8BAR, LFO_TGT_FILTER, depth=70, flt_oct_q=5), trim=0.12)
    rain['lfo2'] = lfo(LFO_TRI, NOTE_DIV_2BAR, LFO_TGT_PAN, depth=60)
    rain['lfo2_authored'] = True
    claim(L, 4, P_NOISE, 72, rain, {0: {}}, follow=FOLLOW_OFF)
    L.update(num_tracks=5, patch=P_SAW_DOWN)
    return L


def arpeggio():
    # Own scale rather than CHORD quant: the chord quantizer maps maj7 to the
    # major scale, which over Gbmaj7 snaps C to Cb against the pads' Bb. F Ab C
    # sits on all four chords as written.
    f = filt(FILTER_LPF, 1400, 1.75, eg1_cutoff=2.0)
    f['eg_depth'][1][EGT_DRIVE] = 0.5
    f['eg_depth'][1][EGT_MIX] = 0.2
    f['eg_depth'][1][EGT_SCAN] = -0.4
    f['key_track'] = 0.85
    a = arp(True, P_WT_CUSTOM, ARP_SLOT, NOTE_DIV_1_16, [65, 68, 72, 53], octaves=2, gate=100,
            quant=ARP_QUANT_OWN, root=65, scale=SCALE, porta=2,
            amp_env=env(2, 152, 4, 235, ENV_DX7), flt=f,
            mod=lfo(LFO_SINE, NOTE_DIV_16BAR, LFO_TGT_FILTER | LFO_TGT_PAN, depth=70,
                    flt_oct_q=6), amp=0.75)
    a['env2'] = env(5, 129, 24, 406)
    a['dist'] = dist(DIST_CRUSH, 2, 25, bits=4, rate=2)
    a['lfo2'] = lfo(LFO_SINE, NOTE_DIV_2BAR,
                    LFO_TGT_AMP | LFO_TGT_SCAN | LFO_TGT_DIST_DRIVE | LFO_TGT_DIST_MIX, depth=10)
    a['lfo2_authored'] = True
    return a


def bass():
    """The stutter drone carries the bass: its bus is the only dry one a
    pitched voice can reach (melodic rows and the arp render on bus 0)."""
    d = drone(True, CHORD_OFF, 41, patch=P_SAW_DOWN, peak=1.0, duck=0.45,
              rate=NOTE_DIV_1_16, sweep=(150.0, 500.0), sweep_bars=16, res=1.2,
              pattern=DRONE_PAT_OFFBEAT, amp_env=env(2, 120, 40, 60), sub=True,
              follow=DRONE_FOLLOW_ROOT)
    d['gate_len'] = 0.85
    return d


PARTS = ('drums', 'pulse', 'pads', 'arp', 'drone')


def build(mutes=()):
    g = glob(BPM, KEY, SCALE,
             bus0=dict(chorus_level=75, chorus_rate=75, chorus_depth=35, chorus_delay=256,
                       reverb_level=50, reverb_liveness=45, reverb_damping=75,
                       echo_level=35, echo_feedback=69, echo_tone=15,
                       echo_div=NOTE_DIV_1_16D, eq_low_db=6, eq_mid_db=4, eq_high_db=-2,
                       bus_dist_type=DIST_CRUSH, bus_dist_drive=1, bus_dist_bits=24,
                       bus_dist_rate=2, bus_dist_mix=25),
             bus1=dict(level=200),
             split=SPLIT_DRUMS | SPLIT_DRONES | SPLIT_CLIPS)
    g['fx'][2]['level'] = 200                   # drone bus: dry
    p = project(g, [drums(), pulse(), pads()], arp_=arpeggio(), drone_=bass(),
                progression=PROGRESSION)
    return apply_mutes(p, PARTS, mutes)
