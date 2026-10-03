"""Reader/writer for S3-Amysynth project files (Pnn.amp).

Mirrors components/synth_core/project/project_snapshot.c field for field:
GLOB v5, LAYR v22, ARP v13, DRON v2, PROG v1, CHRD v1, CLIP v2, PGEN v1,
inside project_store.c's 32-byte header (magic "AMYP", fmt 1, name, len, CRC32).
Field order IS the format; when the firmware bumps a section version, update
the matching read_/write_ pair and VER here (gen_templates.py fails the build
until they agree).

State is plain dicts/lists so a spec can build it declaratively. The helpers
default_* return the firmware's init values (voice_params_init_defaults etc.).
"""
import struct
import zlib

MAGIC = 0x50594D41
FMT_VERSION = 1
NAME_LEN = 16

TAG = {k: struct.unpack('<I', k.encode())[0] for k in ('GLOB', 'LAYR', 'ARP ', 'DRON', 'PROG', 'CHRD', 'CLIP', 'PGEN')}
VER = {'GLOB': 5, 'LAYR': 22, 'ARP ': 13, 'DRON': 2, 'PROG': 1, 'CHRD': 1, 'CLIP': 2, 'PGEN': 1}

SEQ_TRACKS = 5
SEQ_MAX_STEPS = 32
FX_BUS_COUNT = 4
ARP_MAX_SLOTS = 8
SEQ_CHORD_SLOTS = 8
SEQ_CHORD_MAX_NOTES = 5
CLIP_SLOT_COUNT = 2
SEQ_EGT_COUNT = 5
FX_PARAM_UNSET = -32768


class W:
    def __init__(self):
        self.b = bytearray()

    def u8(self, v): self.b += struct.pack('<B', v & 0xFF)
    def i8(self, v): self.b += struct.pack('<b', v)
    def u16(self, v): self.b += struct.pack('<H', v)
    def i16(self, v): self.b += struct.pack('<h', v)
    def u32(self, v): self.b += struct.pack('<I', v)
    def f32(self, v): self.b += struct.pack('<f', v)
    def raw(self, bs): self.b += bytes(bs)

    def section(self, name, body):
        self.u32(TAG[name]); self.u8(VER[name]); self.u32(len(body)); self.raw(body)


class R:
    def __init__(self, b):
        self.b = b; self.p = 0

    def _take(self, fmt):
        n = struct.calcsize(fmt)
        if self.p + n > len(self.b):
            raise ValueError('short read')
        v = struct.unpack_from(fmt, self.b, self.p)[0]; self.p += n
        return v

    def u8(self): return self._take('<B')
    def i8(self): return self._take('<b')
    def u16(self): return self._take('<H')
    def i16(self): return self._take('<h')
    def u32(self): return self._take('<I')
    def f32(self): return self._take('<f')

    def raw(self, n):
        if self.p + n > len(self.b):
            raise ValueError('short read')
        v = self.b[self.p:self.p + n]; self.p += n
        return v

    def done(self): return self.p >= len(self.b)


# ── defaults ────────────────────────────────────────────────────────────────

def default_env():
    return dict(attack_ms=0, decay_ms=0, sustain_pct=0, release_ms=0, eg_type=0)


def default_filter():
    return dict(filter_type=0, cutoff_hz=0.0, resonance=0.0, enabled=False, feedback=0.0,
                eg_depth=[[0.0] * SEQ_EGT_COUNT for _ in range(2)], ks_duty_ofs=0.0)


def default_lfo():
    return dict(enabled=False, mode=0, wave=0, rate=0, depth=0, targets=0,
                wob_rate=0, wob_depth=0, wob_reach=0, flt_oct_q=0)


def default_dist():
    return dict(type=0, drive=2, bits=8, rate=8, mix=100)


def default_vp():
    """voice_params_init_defaults(): zeroed, unity amp_trim, audible dist spare."""
    return dict(env=default_env(), env1=default_env(), filter=default_filter(), lfo=default_lfo(),
                env_authored=False, env1_authored=False, filter_authored=False, lfo_authored=False,
                amp_trim=1.0, dist=default_dist(), dist_authored=False)


def grid(fill=0):
    return [[fill] * SEQ_MAX_STEPS for _ in range(SEQ_TRACKS)]


def default_layer(kind):
    """Neutral per-step values as sequencer_core_add_layer leaves them."""
    return dict(type=kind, num_steps=16, patch=0, synth_flags=0, num_voices=1, chord_mode=False,
                chord_root=0, chord_type=0, swing_pct=0,
                tracks=[dict(base_note=60, patch=0, pcm_preset=0, pcm_mode=0, repeat_rate=1,
                             mute=False, solo=False, follow=0, vp=default_vp())
                        for _ in range(SEQ_TRACKS)],
                grid=grid(0), step_note=grid(60), step_pitch_ofs=grid(0), step_prob=grid(100),
                step_ratchet=grid(1), step_every=grid(1), step_prev=grid(0), step_transform=grid(0),
                step_quant_bypass=grid(0), step_nudge=grid(0), step_velocity_adj=grid(0),
                step_ratchet_taper=grid(0),
                gate_pct=92, portamento_ms=0, groove_pct=100, fm_algo_override=0xFF,
                vp_src=[0] * SEQ_TRACKS, vp_layer=default_vp(), patch_scope=0, num_tracks=4)


# fx_echo_div_t order (amy_fx.h); the index is what the file stores.
ECHO_DIV = {k: i for i, k in enumerate(('1/32', '1/16T', '1/16', '1/8T', '1/8', '1/4T', '1/8D',
                                        '1/4', '1/2T', '1/4D', '1/2'))}


def default_fx():
    return dict(eq_low_db=0, eq_mid_db=0, eq_high_db=0, echo_level=0, chorus_level=0, reverb_level=0,
                echo_delay_ms=FX_PARAM_UNSET, echo_feedback=FX_PARAM_UNSET, echo_tone=FX_PARAM_UNSET,
                echo_sync=1, echo_div=ECHO_DIV['1/8D'],
                reverb_liveness=FX_PARAM_UNSET, reverb_damping=FX_PARAM_UNSET,
                reverb_xover_hz=FX_PARAM_UNSET, chorus_rate=FX_PARAM_UNSET,
                chorus_depth=FX_PARAM_UNSET, bus_dist_type=0, bus_dist_drive=2, bus_dist_bits=8,
                bus_dist_rate=8, bus_dist_mix=100, level=100, chorus_delay=FX_PARAM_UNSET)


# ── sub-block codecs (ser_env / ser_filter / ser_lfo / ser_dist / ser_vp) ──

def w_env(w, e):
    w.u32(e['attack_ms']); w.u32(e['decay_ms']); w.u8(e['sustain_pct'])
    w.u32(e['release_ms']); w.u8(e['eg_type'])


def r_env(r):
    return dict(attack_ms=r.u32(), decay_ms=r.u32(), sustain_pct=r.u8(), release_ms=r.u32(),
                eg_type=r.u8())


def w_filter(w, f):
    w.u8(f['filter_type']); w.f32(f['cutoff_hz']); w.f32(f['resonance'])
    w.u8(1 if f['enabled'] else 0); w.f32(f['feedback'])
    for eg in range(2):
        for t in range(SEQ_EGT_COUNT):
            w.f32(f['eg_depth'][eg][t])
    w.f32(f['ks_duty_ofs'])


def r_filter(r):
    f = dict(filter_type=r.u8(), cutoff_hz=r.f32(), resonance=r.f32(), enabled=r.u8() != 0,
             feedback=r.f32())
    f['eg_depth'] = [[r.f32() for _ in range(SEQ_EGT_COUNT)] for _ in range(2)]
    f['ks_duty_ofs'] = r.f32()
    return f


def w_lfo(w, l):
    w.u8(1 if l['enabled'] else 0)
    for k in ('mode', 'wave', 'rate', 'depth', 'targets', 'wob_rate', 'wob_depth', 'wob_reach',
              'flt_oct_q'):
        w.u8(l[k])


def r_lfo(r):
    l = dict(enabled=r.u8() != 0)
    for k in ('mode', 'wave', 'rate', 'depth', 'targets', 'wob_rate', 'wob_depth', 'wob_reach',
              'flt_oct_q'):
        l[k] = r.u8()
    return l


def w_dist(w, d):
    for k in ('type', 'drive', 'bits', 'rate', 'mix'):
        w.u8(d[k])


def r_dist(r):
    return {k: r.u8() for k in ('type', 'drive', 'bits', 'rate', 'mix')}


def w_vp(w, vp):
    w_env(w, vp['env']); w_env(w, vp['env1']); w_filter(w, vp['filter']); w_lfo(w, vp['lfo'])
    for k in ('env_authored', 'env1_authored', 'filter_authored', 'lfo_authored'):
        w.u8(1 if vp[k] else 0)
    w.f32(vp['amp_trim']); w_dist(w, vp['dist']); w.u8(1 if vp['dist_authored'] else 0)


def r_vp(r):
    vp = dict(env=r_env(r), env1=r_env(r), filter=r_filter(r), lfo=r_lfo(r))
    for k in ('env_authored', 'env1_authored', 'filter_authored', 'lfo_authored'):
        vp[k] = r.u8() != 0
    vp['amp_trim'] = r.f32(); vp['dist'] = r_dist(r); vp['dist_authored'] = r.u8() != 0
    return vp


# ── sections ────────────────────────────────────────────────────────────────

GLOB_FX_FIELDS = [('eq_low_db', 'i8'), ('eq_mid_db', 'i8'), ('eq_high_db', 'i8'),
                  ('echo_level', 'u8'), ('chorus_level', 'u8'), ('reverb_level', 'u8'),
                  ('echo_delay_ms', 'i16'), ('echo_feedback', 'i16'), ('echo_tone', 'i16'),
                  ('echo_sync', 'u8'), ('echo_div', 'u8'),
                  ('reverb_liveness', 'i16'), ('reverb_damping', 'i16'), ('reverb_xover_hz', 'i16'),
                  ('chorus_rate', 'i16'), ('chorus_depth', 'i16'), ('bus_dist_type', 'u8'),
                  ('bus_dist_drive', 'u8'), ('bus_dist_bits', 'u8'), ('bus_dist_rate', 'u8'),
                  ('bus_dist_mix', 'u8'), ('level', 'u8'), ('chorus_delay', 'i16')]


def w_glob(g):
    w = W()
    w.u16(g['bpm']); w.f32(g['master_volume']); w.u8(1 if g['quant_enabled'] else 0)
    w.u8(g['quant_root']); w.u8(g['quant_scale']); w.u8(g['drum_engine'])
    w.u8(1 if g['presets_alter_global'] else 0); w.u8(g['split_flags'])
    for bus in range(FX_BUS_COUNT):
        for k, t in GLOB_FX_FIELDS:
            getattr(w, t)(g['fx'][bus][k])
    return bytes(w.b)


def r_glob(r):
    g = dict(bpm=r.u16(), master_volume=r.f32(), quant_enabled=r.u8() != 0, quant_root=r.u8(),
             quant_scale=r.u8(), drum_engine=r.u8(), presets_alter_global=r.u8() != 0,
             split_flags=r.u8(), fx=[])
    for bus in range(FX_BUS_COUNT):
        g['fx'].append({k: getattr(r, t)() for k, t in GLOB_FX_FIELDS})
    return g


STEP_ARRAYS = [('grid', 'B'), ('step_note', 'B'), ('step_pitch_ofs', 'b'), ('step_prob', 'B'),
               ('step_ratchet', 'B'), ('step_every', 'B'), ('step_prev', 'B'),
               ('step_transform', 'B'), ('step_quant_bypass', 'B'), ('step_nudge', 'b'),
               ('step_velocity_adj', 'b'), ('step_ratchet_taper', 'b')]


def w_layr(L):
    w = W()
    w.u8(L['type']); w.u8(L['num_steps']); w.u16(L['patch']); w.u32(L['synth_flags'])
    w.u8(L['num_voices']); w.u8(1 if L['chord_mode'] else 0); w.u8(L['chord_root'])
    w.u8(L['chord_type']); w.u8(L['swing_pct'])
    for t in range(SEQ_TRACKS):
        T = L['tracks'][t]
        w.u8(T['base_note']); w.u16(T['patch']); w.u16(T['pcm_preset']); w.u8(T['pcm_mode'])
        w.u8(T['repeat_rate']); w.u8(1 if T['mute'] else 0); w.u8(1 if T['solo'] else 0)
        w.u8(T['follow'])
        w_vp(w, T['vp'])
    for k, fmt in STEP_ARRAYS:
        for t in range(SEQ_TRACKS):
            w.raw(struct.pack('<%d%s' % (SEQ_MAX_STEPS, fmt), *[int(x) for x in L[k][t]]))
    w.u16(L['gate_pct']); w.u16(L['portamento_ms']); w.u8(L['groove_pct'])
    w.u8(L['fm_algo_override'])
    for t in range(SEQ_TRACKS):
        w.u8(L['vp_src'][t])
    w_vp(w, L['vp_layer'])
    w.u8(L['patch_scope']); w.u8(L['num_tracks'])
    return bytes(w.b)


def r_layr(r):
    L = dict(type=r.u8(), num_steps=r.u8(), patch=r.u16(), synth_flags=r.u32(), num_voices=r.u8(),
             chord_mode=r.u8() != 0, chord_root=r.u8(), chord_type=r.u8(), swing_pct=r.u8(),
             tracks=[])
    for t in range(SEQ_TRACKS):
        L['tracks'].append(dict(base_note=r.u8(), patch=r.u16(), pcm_preset=r.u16(), pcm_mode=r.u8(),
                                repeat_rate=r.u8(), mute=r.u8() != 0, solo=r.u8() != 0,
                                follow=r.u8(), vp=r_vp(r)))
    for k, fmt in STEP_ARRAYS:
        L[k] = [list(struct.unpack('<%d%s' % (SEQ_MAX_STEPS, fmt), r.raw(SEQ_MAX_STEPS)))
                for _ in range(SEQ_TRACKS)]
    L['gate_pct'] = r.u16(); L['portamento_ms'] = r.u16(); L['groove_pct'] = r.u8()
    L['fm_algo_override'] = r.u8()
    L['vp_src'] = [r.u8() for _ in range(SEQ_TRACKS)]
    L['vp_layer'] = r_vp(r)
    L['patch_scope'] = r.u8(); L['num_tracks'] = r.u8()
    return L


def w_arp(a):
    w = W()
    w.u8(1 if a['enabled'] else 0); w.u16(a['patch']); w.u8(a['dir']); w.u8(a['octaves'])
    w.u8(a['rate']); w.u8(a['gate_pct']); w.u8(a['scale']); w.u8(a['root'])
    w.u16(a['portamento_ms']); w.f32(a['amp_scale'])
    for i in range(ARP_MAX_SLOTS):
        w.i16(a['slots'][i])
    w_env(w, a['env']); w_env(w, a['env2']); w_filter(w, a['filter']); w_lfo(w, a['lfo'])
    w.u8(a['quant_mode']); w_dist(w, a['dist'])
    return bytes(w.b)


def r_arp(r):
    a = dict(enabled=r.u8() != 0, patch=r.u16(), dir=r.u8(), octaves=r.u8(), rate=r.u8(),
             gate_pct=r.u8(), scale=r.u8(), root=r.u8(), portamento_ms=r.u16(), amp_scale=r.f32())
    a['slots'] = [r.i16() for _ in range(ARP_MAX_SLOTS)]
    a['env'] = r_env(r); a['env2'] = r_env(r); a['filter'] = r_filter(r); a['lfo'] = r_lfo(r)
    a['quant_mode'] = r.u8(); a['dist'] = r_dist(r)
    return a


DRON_FIELDS = [('enabled', 'b1'), ('source', 'u8'), ('wave', 'u16'), ('chord', 'u8'), ('root', 'u8'),
               ('patch', 'u16'), ('resonance', 'f32'), ('amp_peak', 'f32'), ('amp_duck', 'f32'),
               ('amp_trim', 'f32'), ('rate', 'u8'), ('sub_enabled', 'b1'), ('sub_interval', 'i8'),
               ('sweep_lo', 'f32'), ('sweep_hi', 'f32'), ('sweep_bars', 'u8'), ('gate_len', 'f32'),
               ('swing', 'u8'), ('blip', 'f32'), ('pattern', 'u8'), ('follow', 'u8')]


def w_dron(d):
    w = W()
    for k, t in DRON_FIELDS:
        if t == 'b1':
            w.u8(1 if d[k] else 0)
        else:
            getattr(w, t)(d[k])
    w_env(w, d['env']); w_env(w, d['env2'])
    return bytes(w.b)


def r_dron(r):
    d = {}
    for k, t in DRON_FIELDS:
        d[k] = (r.u8() != 0) if t == 'b1' else getattr(r, t)()
    d['env'] = r_env(r); d['env2'] = r_env(r)
    return d


def w_prog(p):
    w = W()
    w.u8(1 if p['enabled'] else 0); w.u8(len(p['entries']))
    for root, ct, bars in p['entries']:
        w.u8(root); w.u8(ct); w.u8(bars)
    return bytes(w.b)


def r_prog(r):
    en = r.u8() != 0; n = r.u8()
    return dict(enabled=en, entries=[(r.u8(), r.u8(), r.u8()) for _ in range(n)])


def w_chrd(slots):
    w = W()
    w.u8(SEQ_CHORD_SLOTS)
    for s in range(SEQ_CHORD_SLOTS):
        notes = list(slots[s]) + [0] * (SEQ_CHORD_MAX_NOTES - len(slots[s]))
        w.u8(sum(1 for n in notes if n)); [w.u8(n) for n in notes]
    return bytes(w.b)


def r_chrd(r):
    n = r.u8(); out = []
    for _ in range(n):
        r.u8(); out.append([x for x in (r.u8() for _ in range(SEQ_CHORD_MAX_NOTES))])
    return [[x for x in s if x] for s in out]


def w_clip(c):
    w = W()
    w.u8(c['bars']); w.u8(1 if c['stereo'] else 0); w.u8(c['tail']); w.u8(CLIP_SLOT_COUNT)
    for s in range(CLIP_SLOT_COUNT):
        w.u8(c['level'][s]); w.u8(c['mode'][s])
    w.u8(c['after'])
    return bytes(w.b)


def r_clip(r):
    c = dict(bars=r.u8(), stereo=r.u8() != 0, tail=r.u8())
    n = r.u8(); lv = []; md = []
    for _ in range(n):
        lv.append(r.u8()); md.append(r.u8())
    c['level'] = lv; c['mode'] = md; c['after'] = r.u8()
    return c


def w_pgen(g):
    w = W()
    for k in ('style', 'len', 'bars', 'ext', 'var'):
        w.u8(g[k])
    w.u16(g['seed'])
    return bytes(w.b)


def r_pgen(r):
    g = {k: r.u8() for k in ('style', 'len', 'bars', 'ext', 'var')}
    g['seed'] = r.u16()
    return g


# ── whole project ───────────────────────────────────────────────────────────

def encode_payload(p):
    w = W()
    w.section('GLOB', w_glob(p['glob']))
    for L in p['layers']:
        w.section('LAYR', w_layr(L))
    w.section('ARP ', w_arp(p['arp']))
    w.section('DRON', w_dron(p['drone']))
    w.section('PROG', w_prog(p['prog']))
    w.section('CHRD', w_chrd(p['chords']))
    w.section('CLIP', w_clip(p['clip']))
    w.section('PGEN', w_pgen(p['pgen']))
    return bytes(w.b)


def encode_file(p, name):
    payload = encode_payload(p)
    nm = name.encode()[:NAME_LEN - 1].ljust(NAME_LEN, b'\0')
    hdr = struct.pack('<IHH', MAGIC, FMT_VERSION, 0) + nm + struct.pack('<II', len(payload),
                                                                         zlib.crc32(payload))
    assert len(hdr) == 32
    return hdr + payload


def decode_file(data):
    magic, ver, _res = struct.unpack_from('<IHH', data, 0)
    name = data[8:24].split(b'\0')[0].decode(errors='replace')
    plen, crc = struct.unpack_from('<II', data, 24)
    payload = data[32:32 + plen]
    if magic != MAGIC or ver > FMT_VERSION or zlib.crc32(payload) != crc:
        raise ValueError('bad header/CRC')
    r = R(payload)
    p = dict(name=name, layers=[])
    readers = {'GLOB': ('glob', r_glob), 'ARP ': ('arp', r_arp), 'DRON': ('drone', r_dron),
               'PROG': ('prog', r_prog), 'CHRD': ('chords', r_chrd), 'CLIP': ('clip', r_clip),
               'PGEN': ('pgen', r_pgen)}
    while not r.done():
        tag = r.u32(); ver = r.u8(); blen = r.u32(); body = R(r.raw(blen))
        name4 = struct.pack('<I', tag).decode(errors='replace')
        if name4 in VER and ver != VER[name4]:
            raise ValueError('%s version %d, codec knows %d' % (name4, ver, VER[name4]))
        if name4 == 'LAYR':
            p['layers'].append(r_layr(body))
        elif name4 in readers:
            key, fn = readers[name4]
            p[key] = fn(body)
        if name4 in VER and not body.done():
            raise ValueError('%s body has %d trailing bytes' % (name4, len(body.b) - body.p))
    return p
