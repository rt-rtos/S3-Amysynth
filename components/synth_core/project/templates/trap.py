"""Trap template: 140 BPM (half-time feel), A minor, straight.

808 kit: snare and clap together on beat 3 only, which makes 140 feel like
70; 8th hats with 16th, triplet and 32nd rolls from ratchets. A long 808
bass (sine with a pitch drop, clipped for harmonics, gliding into the octave)
that follows the kick and the chord root, i - VI two bars each. The melody
is the arp in slot mode: a one-bar 8th motif in the key.
"""
from spec_lib import *

NAME = 'TRAP'
BPM = 140
KEY, SCALE = A, SCALE_MINOR
PROGRESSION = [(A, CHORD_MIN, 2), (F, CHORD_MAJ, 2)]

KICK = merge(hits('x.....x..x......'), {14: dict(every=2, vel=-10)})
SNARE = hits('........x.......')
HAT = merge(hits('x.x.x.x.x.x.x.x.', vel=-10),
            {6: dict(ratchet=2, taper=20), 11: dict(vel=-30),
             14: dict(ratchet=3, taper=30),
             15: dict(ratchet=4, taper=-30, every=2, vel=-20)})
CLAP = hits('........x.......', vel=-10)

BASS = line('x.....x..x....x.', [0, 0, 12, 7])
MOTIF = [69, 72, 76, ARP_REST, 74, 72, ARP_REST, 71]   # A4 C5 E5 - D5 C5 - B4


def drums():
    # 808 ROM bank: rows 0 and 2 stay unauthored so the bank's kick and hat
    # tuning seeds them.
    return drum_layer([
        (PCM_808_BD3,  60, KICK,  None),
        (PCM_808_SD2,  52, SNARE, None),
        (PCM_808_HH,   64, HAT,   None),
        (PCM_808_CLAP, 62, CLAP,  None),
    ])


def bass808():
    L = melodic_layer(0, gate=100, groove=30, voices=1, porta=70, chord=PROGRESSION[0][:2])
    claim(L, 0, P_SINE, 33,  # A1
          vp(amp_env=env(1, 0, 100, 1100), eg1=env(0, 60, 0, 40),
             flt=filt(FILTER_LPF, 2000, 0.7, eg1_pitch=1.0),
             dst=dist(DIST_CLIP, 3, 60), trim=0.7),
          BASS, follow=FOLLOW_ROOT)
    L['patch'] = P_SINE
    return L


def melody():
    return arp(True, P_TRIANGLE, ARP_SLOT, NOTE_DIV_1_8, MOTIF, gate=60,
               quant=ARP_QUANT_GLOBAL, root=57, scale=SCALE,
               amp_env=env(8, 300, 40, 250), flt=filt(FILTER_LPF, 3000, 0.8), amp=0.8)


PARTS = ('drums', '808', 'arp')


def build(mutes=()):
    g = glob(BPM, KEY, SCALE,
             bus0=dict(echo_level=15, echo_feedback=35, echo_tone=-20,
                       reverb_level=20))
    p = project(g, [drums(), bass808()], arp_=melody(), progression=PROGRESSION)
    return apply_mutes(p, PARTS, mutes)
