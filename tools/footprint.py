#!/usr/bin/env python3
"""
footprint.py: what qwark.sprx costs the VSH once it is loaded, by area, from a
build's link map and its unstripped module. Needs no SDK; read-only.

Make the two inputs in a Cygwin login shell, from the qwark checkout (the
`qwark.sym` target links exactly what `make` links and leaves the objects, the
.sym and the map behind instead of deleting them):

    make qwark.sym PRX_LDFLAGS_EXTRA='-L ./lib -L $(SDKLIB_DIR) -Wl,--strip-unused-data -Wl,-Map,qwark.map'

then, from Windows or Git Bash:

    python -I tools/footprint.py qwark.map qwark.sym [--against OLD] [--save OUT.json] [--top N] [--files]

and delete objs/, qwark.prx, qwark.sym and qwark.map afterwards. OLD is another
build's map (its .sym beside it, same name, is read when there) or a summary an
earlier run wrote with --save; tools/footprint/ keeps the summaries of earlier
builds (named by build and commit), so a new build compares without rebuilding
an old one.

What it prints:
  - the loaded image by segment, from the module's program headers: RX (code,
    then read-only data), RW (data, then bss, which takes no file bytes);
  - the image by area (per-game code, the menu, the renderers, ...), each split
    into code, read-only data, data, TOC and descriptors, and bss;
  - every static buffer or table of 512 bytes or more;
  - the thread stacks, read from the sources: the module asks the console for
    no other memory once it has loaded (no heap), so the image and the stacks
    are what it holds;
  - with --files, the same per object file; with --top N, the N biggest input
    sections.

Sizes are bytes; KB in the summaries is 1024 bytes. The map lists every input
section the link kept (ps3ppuld writes it with addresses of 0, so a section's
place is not in it; its size and its object are). The areas add up to the
segments' sizes to within the linker's alignment padding, which the summary
shows as its own row.
"""
import json
import os
import re
import struct
import sys
from collections import OrderedDict, defaultdict

CATS = ('code', 'rodata', 'data', 'toc', 'bss')

# Object (path under src/, no extension) -> area. The first match wins.
AREAS = [
    ('per-game: RaC1',            r'^games/rac1(_panel)?$'),
    ('per-game: RaC2',            r'^games/rac2(_panel)?$'),
    ('per-game: UYA',             r'^games/rac3(_panel)?$'),
    ('per-game: Deadlocked',      r'^games/rac4(_panel)?$'),
    ('per-game: shared',          r'^games/(classic|games)$'),
    ('menu model and driver',     r'^core/menu$'),
    ('renderers',                 r'^games/menuhelper_bins$'),
    ('savefile helpers',          r'^games/sfhelper_bins$'),
    ('code switches',             r'^(games/switches_bins|core/switches)$'),
    ('protocol and net',          r'^core/net$'),
    ('session and tick',          r'^core/session$'),
    ('mods',                      r'^core/mods$'),
    ('patch engine (mem.c)',      r'^core/mem$'),
    ('config and positions',      r'^core/config$'),
    ('shared buffers (util.c)',   r'^core/util$'),
    ('features, manips, autosplit, savefile', r'^core/(features|manips|autosplit|savefile)$'),
    ('platform (plat_ps3, printf, libc, main)', r'^(plat/ps3/.*|main)$'),
    ('SDK stubs and module tables', r'^SDK$'),
]

# A renderer's or a savefile helper's own sections name their game.
GAME_OF_SECTION = [('RaC1', r'_rac1_'), ('RaC2', r'_rac2_'), ('UYA', r'_rac3_'), ('Deadlocked', r'_rac4_')]


def category(out_sec):
    """The map's output section name -> one of CATS, or None for what is not loaded."""
    s = out_sec
    if s.startswith('.debug') or s.startswith('.sceversion') or s.startswith('.psp_'):
        return None
    if s.startswith('.rel.') or s.startswith('.rela'):
        return None
    if s.startswith('.text') or s in ('.sceStub.text', '.init', '.fini'):
        return 'code'
    if s.startswith('.rodata') or s.startswith('.lib.'):
        return 'rodata'
    if s.startswith('.bss') or s.startswith('.sbss') or s in ('.common', 'COMMON'):
        return 'bss'
    if s.startswith('.data') or s.startswith('.sdata'):
        return 'data'
    if s in ('.toc', '.opd', '.got', '.toc1', '.tocbss'):
        return 'toc'
    return None


def parse_map(path):
    """Rows of (object, input section, category, size) for every kept input section."""
    rows = []
    out_sec = in_sec = None
    with open(path, encoding='latin-1') as f:
        for ln in f:
            m = re.match(r'^([0-9a-f]{8}) ([0-9a-f]{8}) +\d+ (.*)$', ln.rstrip('\r\n'))
            if not m:
                continue
            size = int(m.group(2), 16)
            rest = m.group(3)
            indent = len(rest) - len(rest.lstrip(' '))
            name = rest.strip()
            if indent == 0:
                out_sec = name
            elif indent == 8:
                in_sec = name
            elif indent == 16:
                obj = re.sub(r' \(TOC#[^)]*\)$', '', name).replace('\\', '/')
                mm = re.search(r'objs/src/(.*)\.ppu\.o$', obj)
                obj = mm.group(1) if mm else 'SDK'
                cat = category(out_sec)
                if cat is not None and size:
                    rows.append((obj, in_sec, cat, size))
    return rows


def area_of(obj):
    for name, rx in AREAS:
        if re.search(rx, obj):
            return name
    return 'other: ' + obj


def segments(path):
    """{'rx': filesz, 'rw_file': filesz, 'rw_mem': memsz, 'rela': bytes} from an ELF64 BE module."""
    with open(path, 'rb') as f:
        d = f.read()
    if d[:4] != b'\x7fELF' or d[4] != 2 or d[5] != 2:
        raise ValueError('%s: not a 64-bit big-endian ELF' % path)
    phoff, = struct.unpack('>Q', d[32:40])
    phentsize, phnum = struct.unpack('>HH', d[54:58])
    seg = {'rx': 0, 'rw_file': 0, 'rw_mem': 0, 'rela': 0}
    for i in range(phnum):
        o = phoff + i * phentsize
        typ, flags, off, vaddr, paddr, filesz, memsz, align = struct.unpack('>IIQQQQQQ', d[o:o + 56])
        if typ == 1 and (flags & 1):
            seg['rx'] += memsz
        elif typ == 1:
            seg['rw_file'] += filesz
            seg['rw_mem'] += memsz
        elif typ == 0x700000a4:
            seg['rela'] += filesz
    return seg


def stacks(srcdir):
    """The thread stacks qwark creates, read from the sources: (name, bytes, per-client)."""
    out = []
    ps3h = os.path.join(srcdir, 'plat', 'ps3', 'qwark_ps3.h')
    thr = os.path.join(srcdir, 'plat', 'ps3', 'thread.h')
    net = os.path.join(srcdir, 'core', 'net.c')
    proto = os.path.join(srcdir, 'core', 'proto.h')
    try:
        ladder = dict((m.group(1), int(m.group(2), 16)) for m in
                      re.finditer(r'#define\s+(THREAD_STACK_SIZE_\w+)\s+0x([0-9A-Fa-f]+)', open(thr).read()))
        names = {'TICK': 'tick thread', 'NET': 'boot and accept thread', 'STOP': 'stop thread'}
        for m in re.finditer(r'#define\s+QWARK_STACK_(\w+)\s+(\w+)', open(ps3h).read()):
            out.append((names.get(m.group(1), m.group(1).lower() + ' thread'), ladder.get(m.group(2), 0), False))
        m = re.search(r'#define\s+CLIENT_STACK\s+(\d+)', open(net).read())
        clients = re.search(r'#define\s+QWARK_MAX_CLIENTS\s+(\d+)', open(proto).read())
        if m:
            out.append(('each connection thread (up to %s)' % (clients.group(1) if clients else '?'),
                        int(m.group(1)), True))
    except (IOError, OSError):
        pass
    return out


SAVED_SECTION_MIN = 256      # what a saved summary keeps one by one


def summarise(rows, seg):
    """
    What the report needs, and what --save writes and --against reads: the
    segments, every object's bytes by category, its string literals, the
    renderers' and savefile helpers' bytes per game, and every input section of
    SAVED_SECTION_MIN bytes or more (all of them while `rows` is in hand).
    """
    files = defaultdict(lambda: dict((c, 0) for c in CATS))
    strings = defaultdict(int)
    sub = OrderedDict()
    for obj, sec, cat, size in rows:
        files[obj][cat] += size
        if re.match(r'\.rodata\.str', sec):
            strings[obj] += size
        a = area_of(obj)
        if a in ('renderers', 'savefile helpers'):
            for g, rx in GAME_OF_SECTION:
                if re.search(rx, sec):
                    key = '%s: %s' % (a, g)
                    sub.setdefault(key, dict((c, 0) for c in CATS))
                    sub[key][cat] += size
    return {'segments': seg, 'files': dict(files), 'strings': dict(strings), 'per_game': sub,
            'sections': [[o, s, c, n] for o, s, c, n in rows]}


def save(summary, path):
    """A summary small enough to commit: the sections under SAVED_SECTION_MIN are left out."""
    keep = dict(summary)
    keep['sections'] = [r for r in summary['sections'] if r[3] >= SAVED_SECTION_MIN]
    with open(path, 'w') as f:
        f.write('{\n')
        keys = sorted(keep.keys())
        for n, k in enumerate(keys):
            end = ',' if n + 1 < len(keys) else ''
            if k == 'sections':
                f.write('"sections": [\n')
                rows = sorted(keep[k], key=lambda r: (-r[3], r[0], r[1]))
                for m, r in enumerate(rows):
                    f.write(' %s%s\n' % (json.dumps(r), ',' if m + 1 < len(rows) else ''))
                f.write(']%s\n' % end)
            else:
                f.write('%s: %s%s\n' % (json.dumps(k), json.dumps(keep[k], sort_keys=True), end))
        f.write('}\n')


def load(path):
    if path.endswith('.json'):
        with open(path) as f:
            return json.load(f, object_pairs_hook=OrderedDict)
    rows = parse_map(path)
    seg = None
    for cand in (os.path.splitext(path)[0] + '.sym', os.path.splitext(path)[0] + '.prx'):
        if os.path.exists(cand):
            seg = segments(cand)
            break
    return summarise(rows, seg)


def kb(n):
    return '%.1f' % (n / 1024.0)


def area_table(summary):
    areas = OrderedDict((name, dict((c, 0) for c in CATS)) for name, _ in AREAS)
    for obj, cats in summary['files'].items():
        a = area_of(obj)
        areas.setdefault(a, dict((c, 0) for c in CATS))
        for c in CATS:
            areas[a][c] += cats.get(c, 0)
    sub = OrderedDict(('  ' + k, v) for k, v in summary.get('per_game', {}).items())
    return areas, sub


def image(c):
    return c['code'] + c['rodata'] + c['data'] + c['toc']


def print_report(cur, old, top, show_files, srcdir):
    seg = cur.get('segments')
    oseg = old.get('segments') if old else None

    def delta(a, b):
        return '' if b is None else '%+8d' % (a - b)

    print('qwark footprint (bytes; KB = 1024 bytes)')
    print()
    if seg:
        rows = [('RX: code and read-only data', seg['rx'], oseg and oseg['rx']),
                ('RW: data', seg['rw_file'], oseg and oseg['rw_file']),
                ('RW: bss', seg['rw_mem'] - seg['rw_file'], oseg and oseg['rw_mem'] - oseg['rw_file']),
                ('loaded image', seg['rx'] + seg['rw_mem'], oseg and oseg['rx'] + oseg['rw_mem']),
                ('relocations (in the file, read at load)', seg['rela'], oseg and oseg['rela'])]
        print('%-44s %9s %8s %9s' % ('segment', 'bytes', 'KB', 'change' if old else ''))
        for name, n, o in rows:
            print('%-44s %9d %8s %9s' % (name, n, kb(n), delta(n, o) if oseg else ''))
        print()

    areas, sub = area_table(cur)
    oareas, osub = area_table(old) if old else ({}, {})
    hdr = '%-42s %7s %7s %6s %6s %8s %7s' % ('area', 'code', 'rodata', 'data', 'toc', 'image', 'bss')
    if old:
        hdr += ' %8s %7s' % ('d.image', 'd.bss')
    print(hdr)
    tot = dict((c, 0) for c in CATS)
    otot = dict((c, 0) for c in CATS)
    names = list(areas.keys()) + [k for k in oareas if k not in areas]
    for name in names:
        c = areas.get(name, dict((k, 0) for k in CATS))
        o = oareas.get(name) if old else None
        if image(c) == 0 and c['bss'] == 0 and (o is None or image(o) == 0 and o['bss'] == 0):
            continue
        line = '%-42s %7d %7d %6d %6d %8d %7d' % (name, c['code'], c['rodata'], c['data'], c['toc'], image(c), c['bss'])
        if old:
            o = o or dict((k, 0) for k in CATS)
            line += ' %+8d %+7d' % (image(c) - image(o), c['bss'] - o['bss'])
        print(line)
        for k in CATS:
            tot[k] += c[k]
            if old:
                otot[k] += (o or {}).get(k, 0)
        if name in ('renderers', 'savefile helpers'):
            for key in list(sub.keys()) + [k for k in osub if k not in sub]:
                if not key.startswith('  ' + name + ':'):
                    continue
                s = sub.get(key, dict((k, 0) for k in CATS))
                line = '%-42s %7d %7d %6d %6d %8d %7d' % (key, s['code'], s['rodata'], s['data'], s['toc'], image(s), s['bss'])
                if old:
                    os_ = osub.get(key, dict((k, 0) for k in CATS))
                    line += ' %+8d %+7d' % (image(s) - image(os_), s['bss'] - os_['bss'])
                print(line)
    line = '%-42s %7d %7d %6d %6d %8d %7d' % ('sum of the sections', tot['code'], tot['rodata'], tot['data'], tot['toc'], image(tot), tot['bss'])
    if old:
        line += ' %+8d %+7d' % (image(tot) - image(otot), tot['bss'] - otot['bss'])
    print(line)
    if seg:
        pad_img = seg['rx'] + seg['rw_file'] - image(tot)
        pad_bss = (seg['rw_mem'] - seg['rw_file']) - tot['bss']
        print('%-42s %7s %7s %6s %6s %8d %7d' % ('alignment and module headers', '', '', '', '', pad_img, pad_bss))
    print()

    strs = cur.get('strings', {})
    if strs:
        by_area = defaultdict(int)
        for obj, n in strs.items():
            by_area[area_of(obj)] += n
        ostr = defaultdict(int)
        for obj, n in (old or {}).get('strings', {}).items():
            ostr[area_of(obj)] += n
        print('string literals (.rodata.str*), included above:')
        for a, n in sorted(by_area.items(), key=lambda kv: -kv[1]):
            print('  %-40s %7d%s' % (a, n, ('  %+d' % (n - ostr.get(a, 0))) if old else ''))
        print('  %-40s %7d%s' % ('all', sum(by_area.values()), ('  %+d' % (sum(by_area.values()) - sum(ostr.values()))) if old else ''))
        print()

    big = [(n, obj, sec, cat) for obj, sec, cat, n in cur['sections'] if cat in ('bss', 'data') and n >= 512]
    oldbig = {}
    for obj, sec, cat, n in (old or {}).get('sections', []):
        oldbig[(obj, sec)] = n
    print('static buffers and tables of 512 bytes or more (bss and data):')
    for n, obj, sec, cat in sorted(big, reverse=True):
        o = oldbig.get((obj, sec))
        ch = '' if not old else ('  new' if o is None else ('  %+d' % (n - o) if n != o else ''))
        print('  %7d  %-5s %-24s %s%s' % (n, cat, obj, re.sub(r'^\.(bss|data)\.', '', sec), ch))
    print()

    st = stacks(srcdir) if srcdir else []
    if st:
        print('thread stacks (from the sources; the console allocates them out of the VSH):')
        fixed = 0
        per = 0
        for name, n, per_client in st:
            print('  %-44s %7d' % (name, n))
            if per_client:
                per = n
            elif not name.startswith('stop'):
                fixed += n
        if seg:
            img = seg['rx'] + seg['rw_mem']
            print('  resident with one client: image %d + stacks %d = %d (%s KB)' % (img, fixed + per, img + fixed + per, kb(img + fixed + per)))
            print('  each further client: %d' % per)
        print('  (the stop thread\'s stack exists only while the module unloads)')
        print()

    if show_files:
        hdr = '%-28s %7s %7s %6s %6s %8s %7s' % ('object', 'code', 'rodata', 'data', 'toc', 'image', 'bss')
        if old:
            hdr += ' %8s %7s' % ('d.image', 'd.bss')
        print(hdr)
        for obj, c in sorted(cur['files'].items(), key=lambda kv: -image(kv[1])):
            o = (old or {}).get('files', {}).get(obj)
            line = '%-28s %7d %7d %6d %6d %8d %7d' % (obj, c['code'], c['rodata'], c['data'], c['toc'], image(c), c['bss'])
            if old:
                o = o or dict((k, 0) for k in CATS)
                line += ' %+8d %+7d' % (image(c) - image(o), c['bss'] - o['bss'])
            print(line)
        print()

    if top:
        print('the %d biggest input sections:' % top)
        rows = sorted(cur['sections'], key=lambda r: -r[3])[:top]
        for obj, sec, cat, n in rows:
            print('  %7d  %-6s %-24s %s' % (n, cat, obj, sec))
        print()


def main(argv):
    args = list(argv)
    opts = {'--against': None, '--save': None, '--top': '0', '--src': None}
    flags = set()
    pos = []
    i = 0
    while i < len(args):
        a = args[i]
        if a in opts:
            opts[a] = args[i + 1]
            i += 2
        elif a in ('--files',):
            flags.add(a)
            i += 1
        elif a in ('-h', '--help'):
            print(__doc__)
            return 0
        else:
            pos.append(a)
            i += 1
    if not pos:
        print(__doc__)
        return 2
    for p in pos[:2] + [x for x in (opts['--against'],) if x]:
        if not os.path.exists(p):
            print('%s: not found' % p)
            return 1
    cur = summarise(parse_map(pos[0]), segments(pos[1]) if len(pos) > 1 else None)
    if not cur['sections']:
        print('%s: no input sections found; is it a ps3ppuld map?' % pos[0])
        return 1
    old = load(opts['--against']) if opts['--against'] else None
    srcdir = opts['--src']
    if srcdir is None:
        guess = os.path.join(os.path.dirname(os.path.abspath(pos[0])), 'src')
        srcdir = guess if os.path.isdir(guess) else None
    print_report(cur, old, int(opts['--top']), '--files' in flags, srcdir)
    if opts['--save']:
        save(cur, opts['--save'])
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
