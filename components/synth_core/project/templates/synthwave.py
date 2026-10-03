"""Synthwave template: 100 BPM, A minor, straight.

LinnDrum kick and hats with the gated-reverb power snare on 2 and 4.
Octave-bouncing 8th saw bass on the chord root, a slow saw pad voiced over
i - VI - III - VII (Am F C G, one bar each) and a two-octave 16th pulse
arpeggio that follows the chord. Chorus, reverb and a dotted-8th echo.
"""
from spec_lib import *

NAME = 'SYNTHWAVE'
BPM = 100
KEY, SCALE = A, SCALE_MINOR
PROGRESSION = [(A, CHORD_MIN, 1), (F, CHORD_MAJ, 1), (C_, CHORD_MAJ, 1), (G, CHORD_MAJ, 1)]

KICK = merge(hits('x.......x.......'), {10: dict(every=2, vel=-10)})
SNARE = hits('....x.......x...')
HAT = hits('x.x.x.x.x.x.x.x.', vel=-15)
OPEN = hits('..............x.', every=2, vel=-15)

BASS = line('x.x.x.x.x.x.x.x.', [0, 12] * 4)
PAD = hits('x...............')


def drums():
    return drum_layer([
        (PCM_LINN_BD,           60, KICK,  None),
        (PCM_POWER_GATED_SNARE, 60, SNARE, None),
        (PCM_LINN_HH,           60, HAT,   vp(flt=filt(FILTER_HPF, 3000, 0.5), melodic=False)),
        (PCM_LINN_OH,           60, OPEN,  vp(trim=0.7, melodic=False)),
    ])


def bass():
    L = melodic_layer(0, gate=50, groove=40, voices=1, chord=PROGRESSION[0][:2])
    claim(L, 0, P_SAW_DOWN, 45,  # A2
          vp(amp_env=env(2, 150, 60, 60), eg1=env(2, 120, 0, 60),
             flt=filt(FILTER_LPF24, 800, 1.0, eg1_cutoff=1.0), dst=dist(DIST_CLIP, 3, 50),
             trim=0.8),
          BASS, follow=FOLLOW_ROOT)
    L['patch'] = P_SAW_DOWN
    return L


def pad():
    L = melodic_layer(0, gate=100, groove=0, voices=2, chord=PROGRESSION[0][:2])
    chord_rows(L, P_SAW_DOWN, [57, 60, 64, 69],   # A3 C4 E4 A4 anchors
               vp(amp_env=env(300, 0, 100, 2200), flt=filt(FILTER_LPF, 2200, 0.7), trim=0.28),
               PAD)
    L['patch'] = P_SAW_DOWN
    return L


def arpeggio():
    return arp(True, P_PULSE, ARP_UP, ARP_RATE_1_16, [57, 60, 64], octaves=2, gate=40,
               quant=ARP_QUANT_CHORD, root=57, scale=SCALE,
               amp_env=env(2, 140, 0, 100), flt=filt(FILTER_LPF, 3000, 1.2, eg1_cutoff=1.0),
               amp=0.6)


PARTS = ('drums', 'bass', 'pad', 'arp')


def build(mutes=()):
    g = glob(BPM, KEY, SCALE,
             bus0=dict(chorus_level=35, reverb_level=30, reverb_liveness=85,
                       echo_level=15, echo_feedback=30, echo_tone=-20),
             bus1=dict(level=200))
    p = project(g, [drums(), bass(), pad()], arp_=arpeggio(), progression=PROGRESSION)
    return apply_mutes(p, PARTS, mutes)
