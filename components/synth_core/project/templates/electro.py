"""Electro template: 128 BPM, E Phrygian, straight.

808 kit: broken kick (1, 2-and-a, 3-and), snare and clap on 2 and 4,
straight 16th hats accented on the 8ths, a cowbell answer. Syncopated saw
bass that locks to the kick and leans on the flat 2nd. A two-octave 16th
arpeggio of the tonic triad. No progression.
"""
from spec_lib import *

NAME = 'ELECTRO'
BPM = 128
KEY, SCALE = E, SCALE_PHRYGIAN

KICK = merge(hits('x.....x...x.....'), {13: dict(every=2, vel=-15)})
SNARE = hits('....x.......x...')
HAT = hits('XxXxXxXxXxXxXxXx', vel=-30)
CLAP = hits('....x.......x...', vel=-15)
BELL = merge(hits('...x.....x......', vel=-20), {9: dict(every=2)})

BASS = line('x..x..x...xx..x.', [0, 0, 12, 0, 1, -2])


def drums():
    # 808 ROM bank: rows 0 and 2 stay unauthored so the bank's kick and hat
    # tuning seeds them.
    return drum_layer([
        (PCM_808_BD1,     56, KICK,  None),
        (PCM_808_SD1,     45, SNARE, None),
        (PCM_808_HH,      62, HAT,   None),
        (PCM_808_CLAP,    62, CLAP,  None),
        (PCM_808_COWBELL, 60, BELL,  None),
    ])


def bass():
    L = melodic_layer(0, gate=50, groove=50, voices=1)
    claim(L, 0, P_SAW_DOWN, 40,               # E2
          vp(amp_env=env(1, 160, 20, 60), eg1=env(1, 120, 0, 60),
             flt=filt(FILTER_LPF24, 600, 2.0, eg1_cutoff=2.0), dst=dist(DIST_CLIP, 4, 60),
             trim=0.8),
          BASS)
    L['patch'] = P_SAW_DOWN
    return L


def arpeggio():
    return arp(True, P_PULSE, ARP_UP, ARP_RATE_1_16, [64, 67, 71], octaves=2, gate=40,
               quant=ARP_QUANT_GLOBAL, root=52, scale=SCALE,
               amp_env=env(2, 120, 0, 90), flt=filt(FILTER_LPF, 2500, 1.5, eg1_cutoff=1.0),
               amp=1.0)


PARTS = ('drums', 'bass', 'arp')


def build(mutes=()):
    g = glob(BPM, KEY, SCALE,
             bus0=dict(echo_level=12, echo_feedback=30, echo_tone=-20,
                       reverb_level=12))
    p = project(g, [drums(), bass()], arp_=arpeggio())
    return apply_mutes(p, PARTS, mutes)
