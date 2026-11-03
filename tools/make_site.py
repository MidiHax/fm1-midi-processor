#!/usr/bin/env python3
"""Refresh the generated parts of the GitHub Pages site in html/.

    python tools/make_site.py              screens and firmware
    python tools/make_site.py --screens    only the screen images
    python tools/make_site.py --firmware   only the firmware and its manifest

Screens: renders every screen in design/fm1-screens.json to
html/img/screens/<slug>.png, at 2x (480 x 480), with the firmware's own 5x7
font. The drawing code is the same as tools/screen-designer.html.

Firmware: copies src/build/dx7/generated/FM1_OFFICIAL_OPEN.fwsc to
html/firmware/FM1-MidiProc.fwsc and writes html/firmware/manifest.json,
which html/flash.html loads. Run build.bat (or fm1_flash.py --build) first.
It also rewrites the README's firmware section (between the
<!-- firmware --> markers): version, build, size, SHA-256 and download link.

Standard library only.
"""
import argparse
import hashlib
import json
import re
import shutil
import struct
import sys
import zlib
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
README = ROOT / 'README.md'
REPO_RAW = 'https://github.com/MidiHax/fm1-midi-processor/raw/main/'
SCREENS = ROOT / 'design/fm1-screens.json'
SITE = ROOT / 'html'
BUILD = ROOT / 'src/build/dx7'

# ---- Firmware font: identical to app/screen.c and the screen designer -------
FONT = [
    [0x3e,0x51,0x49,0x45,0x3e],[0,0x42,0x7f,0x40,0],[0x42,0x61,0x51,0x49,0x46],
    [0x21,0x41,0x45,0x4b,0x31],[0x18,0x14,0x12,0x7f,0x10],[0x27,0x45,0x45,0x45,0x39],
    [0x3c,0x4a,0x49,0x49,0x30],[0x01,0x71,0x09,0x05,0x03],[0x36,0x49,0x49,0x49,0x36],
    [0x06,0x49,0x49,0x29,0x1e],[0x7e,0x11,0x11,0x11,0x7e],[0x7f,0x49,0x49,0x49,0x36],
    [0x3e,0x41,0x41,0x41,0x22],[0x7f,0x41,0x41,0x22,0x1c],[0x7f,0x49,0x49,0x49,0x41],
    [0x7f,0x09,0x09,0x09,0x01],[0x3e,0x41,0x49,0x49,0x7a],[0x7f,0x08,0x08,0x08,0x7f],
    [0,0x41,0x7f,0x41,0],[0x20,0x40,0x41,0x3f,0x01],[0x7f,0x08,0x14,0x22,0x41],
    [0x7f,0x40,0x40,0x40,0x40],[0x7f,0x02,0x0c,0x02,0x7f],[0x7f,0x04,0x08,0x10,0x7f],
    [0x3e,0x41,0x41,0x41,0x3e],[0x7f,0x09,0x09,0x09,0x06],[0x3e,0x41,0x51,0x21,0x5e],
    [0x7f,0x09,0x19,0x29,0x46],[0x46,0x49,0x49,0x49,0x31],[0x01,0x01,0x7f,0x01,0x01],
    [0x3f,0x40,0x40,0x40,0x3f],[0x1f,0x20,0x40,0x20,0x1f],[0x3f,0x40,0x38,0x40,0x3f],
    [0x63,0x14,0x08,0x14,0x63],[0x07,0x08,0x70,0x08,0x07],[0x61,0x51,0x49,0x45,0x43]]
PUNCT = {
    45:[8,8,8,8,8], 47:[0x60,0x18,6,1,0], 43:[8,8,0x3e,8,8], 35:[0x14,0x7f,0x14,0x7f,0x14],
    37:[0x63,0x13,8,0x64,0x63], 62:[0x41,0x22,0x14,8,0], 63:[2,1,0x51,9,6], 33:[0,0,0x5f,0,0],
    34:[0,7,0,7,0], 36:[0x24,0x2a,0x7f,0x2a,0x12], 38:[0x36,0x49,0x55,0x22,0x50], 39:[0,5,3,0,0],
    40:[0,0x1c,0x22,0x41,0], 41:[0,0x41,0x22,0x1c,0], 42:[0x14,8,0x3e,8,0x14], 44:[0,0x50,0x30,0,0],
    46:[0,0x60,0x60,0,0], 58:[0,0x36,0x36,0,0], 59:[0,0x56,0x36,0,0], 60:[8,0x14,0x22,0x41,0],
    61:[0x14,0x14,0x14,0x14,0x14], 64:[0x32,0x49,0x79,0x41,0x3e], 91:[0,0x7f,0x41,0x41,0],
    92:[1,6,0x18,0x60,0], 93:[0,0x41,0x41,0x7f,0], 94:[4,2,1,2,4], 95:[0x40,0x40,0x40,0x40,0x40],
    96:[0,1,2,4,0], 123:[0,8,0x36,0x41,0], 124:[0,0,0x7f,0,0], 125:[0,0x41,0x36,8,0], 126:[8,4,8,0x10,8]}
W = H = 240
ZOOM = 2


def glyph(ch):
    c = ord(ch.upper())
    if 48 <= c <= 57:
        return FONT[c - 48]
    if 65 <= c <= 90:
        return FONT[10 + c - 65]
    return PUNCT.get(c, [0] * 5)


def color(v):
    if v is None:
        return None
    v = int(v, 16) if isinstance(v, str) else int(v)
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return ((r * 527 + 23) >> 6, (g * 259 + 33) >> 6, (b * 527 + 23) >> 6)


class Canvas:
    def __init__(self, bg):
        self.px = [color(bg)] * (W * H)

    def fill(self, x, y, w, h, c):
        for yy in range(max(0, y), min(H, y + h)):
            row = yy * W
            for xx in range(max(0, x), min(W, x + w)):
                self.px[row + xx] = c

    def outline(self, x, y, w, h, t, c):
        self.fill(x, y, w, t, c)
        self.fill(x, y + h - t, w, t, c)
        self.fill(x, y, t, h, c)
        self.fill(x + w - t, y, t, h, c)

    def text(self, x, y, s, scale, c, bg=None, max_chars=0):
        n = min(len(s), max_chars) if max_chars else len(s)
        top = scale // 2
        if bg is not None:
            self.fill(x, y, max(n, max_chars or 0) * 6 * scale, 8 * scale, bg)
        for i in range(n):
            shape, cx = glyph(s[i]), x + i * 6 * scale
            for col in range(5):
                for row in range(7):
                    if shape[col] & (1 << row):
                        self.fill(cx + col * scale, y + top + row * scale, scale, scale, c)

    def element(self, e):
        t = e['type']
        if t == 'text':
            self.text(e['x'], e['y'], e['text'], e.get('scale', 2), color(e['color']), color(e.get('bg')),
                      e.get('maxChars', 0))
        elif t == 'rect':
            if e.get('fill', True):
                self.fill(e['x'], e['y'], e['w'], e['h'], color(e['color']))
            else:
                self.outline(e['x'], e['y'], e['w'], e['h'], max(1, e.get('thickness', 1)), color(e['color']))
        elif t == 'bar':
            if e.get('bg') is not None:
                self.fill(e['x'], e['y'], e['w'], e['h'], color(e['bg']))
            v = max(0, min(100, e.get('value', 50))) / 100
            if e.get('orientation') == 'vertical':
                h = round(e['h'] * v)
                self.fill(e['x'], e['y'] + e['h'] - h, e['w'], h, color(e['color']))
            else:
                self.fill(e['x'], e['y'], round(e['w'] * v), e['h'], color(e['color']))
            if e.get('border') is not None:
                self.outline(e['x'], e['y'], e['w'], e['h'], 1, color(e['border']))
        elif t == 'list':
            lines = e['lines'] if isinstance(e['lines'], list) else e['lines'].split('\n')
            scale, rh, mc = e.get('scale', 2), e.get('rowHeight', 16), e.get('maxChars', 20)
            for r in range(e['rows']):
                y, hl = e['y'] + r * rh, r == e.get('hlRow', -1)
                if hl:
                    self.fill(e['x'], y, mc * 6 * scale, rh, color(e['hlBg']))
                elif e.get('bg') is not None:
                    self.fill(e['x'], y, mc * 6 * scale, rh, color(e['bg']))
                line = lines[r] if r < len(lines) else ''
                self.text(e['x'], y + (rh - 8 * scale) // 2, line, scale,
                          color(e['hlColor'] if hl else e['color']), None, mc)

    def png(self):
        rows = []
        for y in range(H):
            line = bytearray()
            for x in range(W):
                line += bytes(self.px[y * W + x]) * ZOOM
            rows.append(b'\0' + bytes(line))
        raw = b''.join(r for r in rows for _ in range(ZOOM))

        def chunk(kind, data):
            return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
        return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W * ZOOM, H * ZOOM, 8, 2, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def slug(name):
    return re.sub(r'[^a-z0-9]+', '-', name.lower()).strip('-')


def make_screens():
    data = json.loads(SCREENS.read_text(encoding='utf-8'))
    out = SITE / 'img/screens'
    out.mkdir(parents=True, exist_ok=True)
    for s in data['screens']:
        canvas = Canvas(s.get('background', '0x0000'))
        for e in s['elements']:
            canvas.element(e)
        path = out / f'{slug(s["name"])}.png'
        path.write_bytes(canvas.png())
        print(f'  {path.relative_to(ROOT)}')


def make_firmware():
    package = BUILD / 'generated/FM1_OFFICIAL_OPEN.fwsc'
    identity = BUILD / 'build-identity.json'
    report = BUILD / 'firmware-report.json'
    for f in (package, identity, report):
        if not f.exists():
            sys.exit(f'Missing {f}. Run src/build.bat first.')
    if not json.loads(report.read_text(encoding='utf-8-sig')).get('passed'):
        sys.exit('firmware-report.json says the build failed its checks.')
    ident = json.loads(identity.read_text(encoding='utf-8-sig'))
    version_h = (ROOT / 'src/app/version.h').read_text(encoding='utf-8')
    product = re.search(r'#define FM1_UPDATE_PRODUCT "([^"]+)"', version_h).group(1)
    version = re.search(r'#define MIDIPROC_VERSION "([^"]+)"', version_h).group(1)
    raw = package.read_bytes()
    out = SITE / 'firmware'
    out.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(package, out / 'FM1-MidiProc.fwsc')
    manifest = {
        'schema': 1,
        'name': 'FM-1 MIDI Processor',
        'version': version,
        'file': 'FM1-MidiProc.fwsc',
        'product': product,
        'release_version': ident['release_version'],
        'build_id': ident['build_id'],
        'family': 7,
        'bytes': len(raw),
        'sha256': hashlib.sha256(raw).hexdigest(),
        'built': datetime.fromtimestamp(package.stat().st_mtime, timezone.utc).strftime('%Y-%m-%d %H:%M UTC'),
    }
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print(f'  html/firmware/FM1-MidiProc.fwsc  {len(raw)} bytes  version {version}  build {ident["build_id"][:16]}')
    update_readme(manifest)


def update_readme(m):
    """Rewrite the README's firmware section from the manifest."""
    text = README.read_text(encoding='utf-8')
    block = (
        '<!-- firmware: written by tools/make_site.py --firmware -->\n'
        f'**Latest version: {m["version"]}** (build `{m["build_id"][:16]}`, {m["built"]})\n'
        '\n'
        f'- **[FM1-MidiProc.fwsc]({REPO_RAW}html/firmware/{m["file"]})**: the compiled firmware package, '
        f'{m["bytes"]:,} bytes, for use with your own flasher.\n'
        f'- SHA-256: `{m["sha256"]}`\n'
        f'- Update identity `{m["product"]}`, FM1 Open SDK family {m["family"]}. '
        f'Details in [manifest.json]({REPO_RAW}html/firmware/manifest.json).\n'
        '<!-- /firmware -->')
    new, n = re.subn(r'<!-- firmware[^>]*-->.*?<!-- /firmware -->', lambda _: block, text, flags=re.S)
    if n != 1:
        print('  README.md: firmware markers not found, not updated')
        return
    if new != text:
        README.write_text(new, encoding='utf-8')
    print('  README.md firmware section updated')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--screens', action='store_true', help='only render the screen images')
    ap.add_argument('--firmware', action='store_true', help='only copy the firmware and write its manifest')
    args = ap.parse_args()
    both = not (args.screens or args.firmware)
    if both or args.screens:
        print('Screens:')
        make_screens()
    if both or args.firmware:
        print('Firmware:')
        make_firmware()
    (SITE / '.nojekyll').touch()
    return 0


if __name__ == '__main__':
    sys.exit(main())
