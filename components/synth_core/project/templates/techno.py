"""Techno template: 132 BPM, A minor, straight.

909 kick on every beat, clap on 2 and 4, 16th closed hats accented on the
off-beats, open hat on the 8th off-beats. Rolling bass: three short 16ths
between each kick. A minor-triad stab every three 16ths against the 4/4,
with a 3/16 echo and a 4-bar filter sweep. No progression: one key, one
chord.
"""
from spec_lib import *

NAME = 'TECHNO'
BPM = 132
KEY, SCALE = A, SCALE_MINOR
SLOT_STAB = 0
CHORDS = {SLOT_STAB: [57, 60, 64]}          # A3 C4 E4

KICK = hits('x...x...x...x...')
CLAP = hits('....x.......x...', vel=-10)
HAT = merge(hits('xxxxxxxxxxxxxxxx', vel=-35),
            {s: dict(vel=-10) for s in (2, 6, 10, 14)})
OPEN = hits('..x...x...x...x.', vel=-5)

BASS = merge(hits('.xxx.xxx.xxx.xxx'), {15: dict(ofs=12, every=2)})
STAB = hits('..x..x..x..x..x.')


def drums():
    return drum_layer([
        (PCM_909_BD,       58, KICK, None),
        (PCM_909_CLAP,     60, CLAP, None),
        (PCM_909_HH_SHORT, 62, HAT,  vp(flt=filt(FILTER_HPF, 5000, 0.7), melodic=False)),
        (PCM_909_OH_SHORT, 60, OPEN, vp(flt=filt(FILTER_HPF, 3000, 0.7), trim=0.8, melodic=False)),
    ])


def bass():
    L = melodic_layer(0, gate=50, groove=40, voices=1)
    claim(L, 0, P_SAW_DOWN, 33,               # A1
          vp(amp_env=env(1, 90, 0, 40), eg1=env(1, 70, 0, 40),
             flt=filt(FILTER_LPF24, 300, 0.8, eg1_cutoff=1.2), dst=dist(DIST_CLIP, 4, 60),
             trim=1.0),
          BASS)
    L['patch'] = P_SAW_DOWN
    return L


def stab():
    L = melodic_layer(0, gate=30, groove=50, voices=2)
    claim(L, 0, P_SAW_DOWN, SLOT_STAB,
          vp(amp_env=env(2, 200, 0, 150), eg1=env(2, 150, 0, 100),
             flt=filt(FILTER_LPF, 900, 2.5, eg1_cutoff=2.0),
             mod=lfo(LFO_SINE, LFO_4BAR, LFO_TGT_FILTER, depth=60, flt_oct_q=5), trim=0.6),
          STAB)
    L['patch'] = P_SAW_DOWN
    return L


PARTS = ('drums', 'bass', 'stab')


def build(mutes=()):
    g = glob(BPM, KEY, SCALE,
             bus0=dict(echo_level=20, echo_feedback=40, echo_tone=-30,
                       reverb_level=18),
             bus1=dict(eq_low_db=2, bus_dist_type=DIST_CLIP, bus_dist_drive=2, bus_dist_mix=30,
                       level=170))
    p = project(g, [drums(), bass(), stab()], chords=CHORDS)
    return apply_mutes(p, PARTS, mutes)
