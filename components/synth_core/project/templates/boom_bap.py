"""Boom bap template: 90 BPM, D minor, heavy swing.

LinnDrum kit, MPC-style swing (about 60 %): kick on 1, the a of 2 and the
and of 3; snare on 2 and 4 with a ghost; 8th hats with 16th ghosts. Four-row
electric-piano chords (sine, clip grit, tremolo) voiced over i - iv - VI - V7,
one bar each; a gritty sine bass that follows the kick and the chord roots.
"""
from spec_lib import *

NAME = 'BOOM BAP'
BPM = 90
KEY, SCALE = D, SCALE_MINOR
PROGRESSION = [(D, CHORD_MIN7, 1), (G, CHORD_MIN7, 1), (Bb, CHORD_MAJ7, 1), (A, CHORD_DOM7, 1)]
SLOT_BASS = 0
CHORDS = {SLOT_BASS: [38]}                  # D2
SWING = 20

KICK = merge(hits('x......x..x.....'), {15: dict(every=2, vel=-20)})
SNARE = merge(hits('....x.......x...'), {15: dict(vel=-45, prob=50)})
HAT = merge(hits('x.x.x.x.x.x.x.x.', vel=-15),
            {5: dict(vel=-40, prob=50), 13: dict(vel=-40, prob=50)})
OPEN = hits('..............x.', every=2, vel=-20)

KEYS = merge(hits('x.........x.....'), {10: dict(vel=-15)})
BASS = line('x......x..x.....', [0, 7, 0])


def drums():
    return drum_layer([
        (PCM_LINN_BD, 60, KICK,  None),
        (PCM_LINN_SD, 60, SNARE, None),
        (PCM_LINN_HH, 60, HAT,   vp(flt=filt(FILTER_HPF, 3000, 0.5), melodic=False)),
        (PCM_LINN_OH, 60, OPEN,  vp(trim=0.7, melodic=False)),
    ], swing=SWING)


def bass():
    L = melodic_layer(SWING, gate=80, groove=60, voices=1, porta=30, chord=PROGRESSION[0][:2])
    claim(L, 0, P_SINE, SLOT_BASS,
          vp(amp_env=env(3, 400, 50, 150), dst=dist(DIST_CLIP, 3, 50), trim=0.6),
          BASS)
    L['patch'] = P_SINE
    return L


def keys():
    L = melodic_layer(SWING, gate=100, groove=60, voices=2, chord=PROGRESSION[0][:2])
    chord_rows(L, P_SINE, [53, 57, 60, 64],   # F3 A3 C4 E4 anchors
               vp(amp_env=env(3, 1400, 0, 900), dst=dist(DIST_CLIP, 3, 40),
                  mod=lfo(LFO_SINE, LFO_1_4, LFO_TGT_AMP, depth=25), trim=0.25),
               KEYS)
    L['patch'] = P_SINE
    return L


PARTS = ('drums', 'bass', 'keys')


def build(mutes=()):
    g = glob(BPM, KEY, SCALE, bus0=dict(eq_high_db=-2, reverb_level=15),
             bus1=dict(eq_high_db=-2, bus_dist_type=DIST_CLIP, bus_dist_drive=2,
                       bus_dist_mix=30, level=200))
    p = project(g, [drums(), bass(), keys()], progression=PROGRESSION, chords=CHORDS)
    return apply_mutes(p, PARTS, mutes)
