"""Dub techno template: 120 BPM, C minor, straight.

Soft low 909 kick on every beat, rim on 2 and 4, quiet 16th hats with the
open hat on the off-beats. One minor-7th chord stab through a slowly swept
low-pass into a long, dark dotted-8th echo and a big reverb - the echo is
the instrument. Sine sub on the off-beats. No progression.
"""
from spec_lib import *

NAME = 'DUB TECHNO'
BPM = 120
KEY, SCALE = C_, SCALE_MINOR
SLOT_CHORD = 0
CHORDS = {SLOT_CHORD: [55, 58, 60, 63]}     # G3 Bb3 C4 Eb4, Cm7

KICK = hits('x...x...x...x...')
RIM = hits('....x.......x...', vel=-20)
HAT = merge(hits('xxxxxxxxxxxxxxxx', vel=-45), {s: dict(prob=70) for s in range(1, 16, 2)})
OPEN = hits('..x...x...x...x.', vel=-15)

CHORD = merge(hits('..x.......x..x..'), {13: dict(prob=50, vel=-15)})
SUB = hits('..x...x...x...x.', vel=-10)


def drums():
    return drum_layer([
        (PCM_909_BD_LO,    60, KICK, None),
        (PCM_909_RIM,      60, RIM,  None),
        (PCM_909_HH_SHORT, 62, HAT,  vp(flt=filt(FILTER_HPF, 6000, 0.7), melodic=False)),
        (PCM_909_OH_SHORT, 58, OPEN, vp(flt=filt(FILTER_HPF, 4000, 0.7), trim=0.7, melodic=False)),
    ])


def chords():
    L = melodic_layer(0, gate=40, groove=40, voices=3)
    claim(L, 0, P_SAW_DOWN, SLOT_CHORD,
          vp(amp_env=env(2, 250, 0, 300), eg1=env(2, 180, 0, 150),
             flt=filt(FILTER_LPF, 700, 2.0, eg1_cutoff=1.5),
             mod=lfo(LFO_SINE, LFO_4BAR, LFO_TGT_FILTER, depth=70, flt_oct_q=6), trim=0.8),
          CHORD)
    claim(L, 1, P_SINE, 36,                   # C2
          vp(amp_env=env(4, 150, 70, 80), trim=0.5), SUB)
    L['patch'] = P_SAW_DOWN
    return L


PARTS = ('drums', 'chords')


def build(mutes=()):
    g = glob(BPM, KEY, SCALE,
             bus0=dict(eq_low_db=-3, echo_level=45, echo_feedback=62,
                       echo_tone=-40, reverb_level=35, reverb_liveness=85, reverb_damping=60),
             bus1=dict(level=150))
    p = project(g, [drums(), chords()], chords=CHORDS)
    return apply_mutes(p, PARTS, mutes)
