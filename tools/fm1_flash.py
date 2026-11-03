#!/usr/bin/env python3
"""Flash FM-1 MIDI Processor firmware over USB MIDI, without the web page.

A Python port of the SDK's browser updater (src/sdk/WebUpdate.js): the same
00 59 SysEx protocol, package checks and identity checks. Run it from any
directory:

    python tools/fm1_flash.py            flash src/build/dx7/generated/FM1_OFFICIAL_OPEN.fwsc
    python tools/fm1_flash.py --build    run src/build.bat first, then flash
    python tools/fm1_flash.py --info     show the device identity and build, change nothing
    python tools/fm1_flash.py --list     list MIDI ports

Requires python-rtmidi (pip install python-rtmidi). Close every other program
that holds the FM-1's MIDI port first (the updater web page, MIDI-OX, a DAW):
Windows lets only one program open it.

Flow (tagged mode, device already runs Open-based firmware):
  identity (17) -> capability (49) -> arm with size and build id (50)
  -> enter loader (F0 22 24 35 7F F7) -> serve image until 0xE0000000
  -> device reboots into the vendor loader (identity ota-FM-1_015)
  -> enter loader again -> serve image until 0xF0000000 (Flash write)
  -> device reboots into the new firmware -> capability must report the new build.
"""
import argparse
import hashlib
import json
import queue
import re
import subprocess
import sys
import time
from pathlib import Path

try:
    import rtmidi
except ImportError:
    sys.exit('python-rtmidi is missing: pip install python-rtmidi')

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / 'src'
DEFAULT_FIRMWARE = SRC / 'build/dx7/generated/FM1_OFFICIAL_OPEN.fwsc'
VENDOR_LOADER_SHA = 'ef1f2db89b011fa53e71df363f0122de0ea896618c53fb53e8da676322004541'
FAMILY = 7
ENTER_LOADER = [0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7]
FINISH_PREPARE, FINISH_WRITE = 0xE0000000, 0xF0000000
SUCCESS = list(b'success\0')

# Timings from WebUpdate.js (seconds).
IDLE, WRITE_IDLE, RECONNECT, SETTLE = 10.0, 180.0, 40.0, 0.6

OPEN_IDENTITY = re.compile(r'^(FM1_OFFICIAL_OPEN|FM1_COMMUNITY_\d+|FM-1_8\d{6})$')
DEVICE_NAME = re.compile(r'fm.?1|^usb.?midi$', re.I)
LOADER_NAME = re.compile(r'fm.?1|felucca|sloop|clipper|ota|sinco|usb.?midi|composite', re.I)


class FlashError(Exception):
    pass


def check(ok, message):
    if not ok:
        raise FlashError(message)


# ---- Package format (inspectPackage in WebUpdate.js) ----------------------
def crc16(data):
    c = 0
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021 if c & 0x8000 else c << 1) & 0xFFFF
    return c


def decode_header(data, key=0xFFFF):
    out = bytearray(data)
    for i in range(len(out)):
        out[i] ^= key & 0xFF
        key = ((key << 1) ^ (0x1021 if key & 0x8000 else 0)) & 0xFFFF
    return out


def le(data, at, n):
    return int.from_bytes(data[at:at + n], 'little')


def ascii_z(data):
    return bytes(data).split(b'\0', 1)[0].decode('ascii', 'replace')


def inspect_package(raw):
    """Return (product, image, loader) after the browser updater's checks."""
    check(1024 <= len(raw) <= 2097152, 'Package length is invalid.')
    image = bytearray(len(raw) - 20)
    product, ended = '', False
    for i in range(20):
        image[i * 47:i * 47 + 47] = raw[i * 48:i * 48 + 47]
        marker = raw[i * 48 + 47]
        if marker == 125:
            ended = True
        else:
            c = (marker - i - 1) & 0xFF
            check(not ended and 32 <= c <= 126, 'Package identity is invalid.')
            product += chr(c)
    image[940:] = raw[960:]
    head = decode_header(image[:64])
    count = le(head, 8, 2)
    check(le(head, 0, 2) == crc16(head[2:]) and le(head, 4, 4) == len(image), 'Package header check failed.')
    check(ascii_z(head[16:32]) == 'AC791N' and 0 < count <= 12, 'Chip model or package table is invalid.')
    table = image[64:64 + count * 80]
    check(crc16(table) == le(head, 2, 2), 'Package table check failed.')
    entries = []
    for i in range(count):
        e = decode_header(table[i * 80:i * 80 + 80])
        kind, offset, size = le(e, 0, 2), le(e, 8, 4), le(e, 12, 4)
        stored, encrypted, name = le(e, 16, 4), le(e, 24, 4), ascii_z(e[64:])
        check(offset >= 64 + count * 80 and offset <= len(image) and size <= len(image) - offset, 'Package entry out of range.')
        check(size <= stored <= len(image) - offset and (not encrypted or encrypted == stored), 'Package entry size is invalid.')
        data = bytes(image[offset:offset + size])
        check(encrypted or crc16(data) == le(e, 4, 2), f'Package entry CRC failed: {name}')
        check(not encrypted or kind not in (0, 100), 'Main image or loader is encrypted.')
        entries.append((kind, size, data))
    flash = [e for e in entries if e[0] == 0]
    loader = [e for e in entries if e[0] == 100]
    check(len(flash) == 1 and 16384 <= flash[0][1] <= 1044480 and flash[0][1] % 4096 == 0, 'Flash image is invalid.')
    check(len(loader) == 1 and 0 < loader[0][1] < 32768, 'Loader is missing or invalid.')
    return product, bytes(image), loader[0][2]


# ---- Update frames (frame/parse in WebUpdate.js) ---------------------------
def pack7(data):
    out, bits, value = [], 0, 0
    for b in data:
        value |= b << bits
        bits += 8
        while bits >= 7:
            out.append(value & 127)
            value >>= 7
            bits -= 7
    if bits:
        out.append(value & 127)
    return out


def frame(command, payload=()):
    payload = list(payload)
    check(len(payload) <= 520, 'Frame payload too long.')
    total = sum(payload) & 0xFF
    body = [0, 89, command, len(payload) & 0xFF, len(payload) >> 8, 0] + payload + [(~total) & 0xFF]
    return [0xF0] + pack7(body) + [0xF7]


def parse(msg):
    if not msg or len(msg) < 10 or len(msg) > 608 or msg[0] != 0xF0 or msg[-1] != 0xF7:
        return None
    raw, bits, value = [], 0, 0
    for b in msg[1:-1]:
        if b > 127:
            return None
        value |= b << bits
        bits += 7
        if bits >= 8:
            raw.append(value & 0xFF)
            value >>= 8
            bits -= 8
    if bits >= 7 or value or len(raw) < 7 or raw[0] != 0 or raw[1] != 89 or le(raw, 3, 3) != len(raw) - 7:
        return None
    if sum(raw[6:]) & 0xFF != 0xFF:
        return None
    return raw[2], bytes(raw[6:-1])


# ---- MIDI link ------------------------------------------------------------
def base_name(port):
    """WinMM appends the port index: 'USB-Midi 4' -> 'USB-Midi'."""
    return re.sub(r'\s+\d+$', '', port)


def port_pairs(pattern, wanted=None):
    """Matching (input index, output index, name) pairs, by base name."""
    mi, mo = rtmidi.MidiIn(), rtmidi.MidiOut()
    try:
        ins, outs = mi.get_ports(), mo.get_ports()
    finally:
        mi.delete()
        mo.delete()
    used, pairs = set(), []
    for i, name in enumerate(ins):
        base = base_name(name)
        if wanted is not None:
            if wanted.lower() not in name.lower():
                continue
        elif not pattern.search(base):
            continue
        for o, out in enumerate(outs):
            if o not in used and base_name(out) == base:
                used.add(o)
                pairs.append((i, o, base))
                break
    return pairs


class Link:
    def __init__(self, in_index, out_index, name):
        self.name = name
        self.queue = queue.Queue()
        self.mi, self.mo = rtmidi.MidiIn(), rtmidi.MidiOut()
        try:
            self.mi.ignore_types(sysex=False, timing=True, active_sense=True)
            self.mi.open_port(in_index)
            self.mo.open_port(out_index)
        except rtmidi.RtMidiError as e:
            self.close()
            raise FlashError(f'Cannot open "{name}" ({e}). Close the updater page, MIDI-OX, '
                             'DAWs and anything else using the FM-1 MIDI port.') from e
        self.mi.set_callback(self._receive)

    def _receive(self, event, _data=None):
        packet = parse(event[0])
        if packet:
            self.queue.put(packet)

    def send(self, data):
        self.mo.send_message(data)

    def read(self, timeout):
        try:
            return self.queue.get(timeout=max(0.0, timeout))
        except queue.Empty:
            return None

    def flush(self):
        while not self.queue.empty():
            self.queue.get_nowait()

    def query(self, command, payload=(), timeout=1.5):
        self.flush()
        self.send(frame(command, payload))
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            reply = self.read(min(0.25, end - time.monotonic()))
            if reply and reply[0] == command:
                return reply[1]
        return None

    def close(self):
        for port in (self.mi, self.mo):
            try:
                port.close_port()
                port.delete()
            except Exception:
                pass  # The device may already have re-enumerated.


def identify(link):
    for _ in range(2):
        reply = link.query(17, timeout=0.75)
        if reply is not None:
            check(len(reply) == 27 and all(32 <= b <= 126 for b in reply.rstrip(b'\0')) and ascii_z(reply),
                  'Device sent an invalid identity reply.')
            return ascii_z(reply)
    return None


def discover(pattern, wanted=None, quiet=True):
    """Open every candidate port pair, keep the one that answers the identity query."""
    found = []
    for in_index, out_index, name in port_pairs(pattern, wanted):
        try:
            link = Link(in_index, out_index, name)
        except FlashError as e:
            if not quiet:
                print(f'  {e}')
            continue
        identity = identify(link)
        if identity:
            found.append((link, identity))
        else:
            link.close()
    if len(found) > 1:
        for link, _ in found:
            link.close()
        raise FlashError('More than one device answered. Connect only one FM-1, or pick one with --port.')
    return found[0] if found else (None, None)


def capability(data):
    check(data is not None, 'Device did not answer the capability query.')
    check(len(data) == 47 and data[:4] == b'FMC1' and data[4] == 1 and data[5] <= 7
          and le(data, 8, 2) == 512 and le(data, 10, 4) == 1048576, 'Device update protocol does not match.')
    check(data[7] == 1, 'Device uses an unknown loader.')
    return {'family': data[5], 'state': data[6], 'build': data[14:46].hex(), 'error': data[46]}


# ---- Update ---------------------------------------------------------------
_last_progress = None


def progress(label, done, total):
    global _last_progress
    pct = min(99, done * 100 // total)
    if (label, pct) == _last_progress:
        return
    _last_progress = (label, pct)
    bar = '#' * (pct // 4)
    print(f'\r  {label:<18} [{bar:<25}] {pct:3d}%', end='', flush=True)


def serve(link, image, finish, idle, label):
    """Answer the loader's block requests (command 48) until it asks for `finish`."""
    covered = bytearray((len(image) + 7) // 8)
    total, started = 0, time.monotonic()
    last = started
    while True:
        now = time.monotonic()
        check(now - started < max(idle * 3, 120), f'{label}: took too long.')
        check(now - last < idle, f'{label}: device stopped requesting data.')
        packet = link.read(0.25)
        if not packet or packet[0] != 48 or len(packet[1]) != 8:
            continue
        p = packet[1]
        address, count = le(p, 1, 4), le(p, 5, 3)
        check(0 < count <= 512, 'Device requested an invalid length.')
        if address in (FINISH_PREPARE, FINISH_WRITE):
            check(address == finish and count == 8, 'Device is in an unexpected update phase.')
            link.send(frame(48, list(p) + SUCCESS))
            end = time.monotonic() + SETTLE
            while time.monotonic() < end:  # answer repeated finish requests
                again = link.read(min(0.1, end - time.monotonic()))
                if again and again[0] == 48 and len(again[1]) == 8 and le(again[1], 1, 4) == finish:
                    link.send(frame(48, list(again[1]) + SUCCESS))
            print(f'\r  {label:<18} [{"#" * 25}] 100%')
            return
        check(address <= len(image) and count <= len(image) - address, 'Device read past the end of the image.')
        last = time.monotonic()
        link.send(frame(48, list(p) + list(image[address:address + count])))
        for i in range(address, address + count):
            if not covered[i >> 3] & (1 << (i & 7)):
                covered[i >> 3] |= 1 << (i & 7)
                total += 1
        progress(label, total, len(image))


def wait_for(identity, any_loader=False, wanted=None):
    end = time.monotonic() + RECONNECT
    while time.monotonic() < end:
        link, found = discover(LOADER_NAME, wanted)
        if link:
            if found == identity or (any_loader and found.lower().startswith('ota-')):
                return link
            link.close()
        time.sleep(SETTLE)
    raise FlashError(f'Device did not come back as {identity} within {RECONNECT:.0f} s. '
                     'Replug USB and run the script again: it resumes from the loader.')


def load_package(path):
    raw = Path(path).read_bytes()
    product, image, loader = inspect_package(raw)
    check(OPEN_IDENTITY.match(product), f'Package identity {product} is not an Open-based firmware.')
    identity_file = Path(path).resolve().parent.parent / 'build-identity.json'
    check(identity_file.exists(), f'Missing {identity_file}. Rebuild with build.bat.')
    build_id = json.loads(identity_file.read_text(encoding='utf-8-sig'))['build_id']
    check(re.fullmatch(r'[0-9a-f]{64}', build_id), 'build-identity.json has an invalid build_id.')
    report = identity_file.parent / 'firmware-report.json'
    if report.exists():
        check(json.loads(report.read_text(encoding='utf-8-sig')).get('passed'), 'firmware-report.json says the build failed its checks.')
    vendor = hashlib.sha256(loader).hexdigest() == VENDOR_LOADER_SHA
    loader_identity = 'ota-FM-1_015' if vendor else 'ota-FM1C_' + build_id[:16]
    return {'product': product, 'image': image, 'build_id': build_id, 'loader_identity': loader_identity,
            'sha256': hashlib.sha256(raw).hexdigest(), 'bytes': len(raw)}


def flash(pkg, force=False, wanted=None):
    link, identity = discover(LOADER_NAME if force else DEVICE_NAME, wanted, quiet=False)
    if not link and not force:  # an interrupted update leaves the vendor loader running
        link, identity = discover(LOADER_NAME, wanted, quiet=False)
    check(link, 'No FM-1 answered. Is it on, connected by USB, and is no other program using its MIDI port?')
    try:
        print(f'Device: {identity} on "{link.name}"')
        loader = identity == pkg['loader_identity'] or (force and identity.lower().startswith('ota-'))
        if loader:
            print('Device is in the loader (interrupted update?). Resuming the Flash write.')
        else:
            legacy = False
            if not OPEN_IDENTITY.match(identity):
                check(force, f'Device runs {identity}, not Open-based firmware. Use --force to replace it '
                             '(stock V15 projects, voices and samples may not survive; back them up first).')
                legacy = True
            if not legacy:
                cap = capability(link.query(49))
                check(cap['family'] == FAMILY or force, f'Device family {cap["family"]} is not {FAMILY}.')
                check(cap['state'] == 0, 'Device is busy with another update.')
                print(f'Installed build: {cap["build"][:16]}   new build: {pkg["build_id"][:16]}')
                if cap['build'] == pkg['build_id']:
                    print('The device already runs this build. Nothing to do.')
                    return
                n, tag = len(pkg['image']), list(bytes.fromhex(pkg['build_id']))
                ack = link.query(50, [1, cap['family'], n & 255, (n >> 8) & 255, (n >> 16) & 255, (n >> 24) & 255] + tag)
                check(ack == b'\0', 'Device refused this package.')
            link.flush()
            link.send(ENTER_LOADER)
            serve(link, pkg['image'], FINISH_PREPARE, IDLE, 'Preparing loader')
            link.close()
            print('  Waiting for the loader to reconnect...')
            link = wait_for(pkg['loader_identity'], any_loader=legacy, wanted=wanted)
        link.flush()
        link.send(ENTER_LOADER)
        serve(link, pkg['image'], FINISH_WRITE, WRITE_IDLE, 'Writing firmware')
        link.close()
        print('  Waiting for the device to restart...')
        link = wait_for(pkg['product'], wanted=wanted)
        cap = capability(link.query(49))
        check(cap['family'] == FAMILY and cap['build'] == pkg['build_id'] and cap['state'] == 0,
              f'Device restarted, but reports build {cap["build"][:16]} instead of {pkg["build_id"][:16]}.')
        print(f'Done. Device runs build {cap["build"][:16]}.')
    finally:
        link.close()


def info(wanted=None):
    link, identity = discover(LOADER_NAME, wanted, quiet=False)
    check(link, 'No FM-1 answered.')
    try:
        print(f'Port:     {link.name}\nIdentity: {identity}')
        if OPEN_IDENTITY.match(identity):
            cap = capability(link.query(49))
            print(f'Family:   {cap["family"]}\nState:    {cap["state"]}\nBuild:    {cap["build"]}')
    finally:
        link.close()


def main():
    ap = argparse.ArgumentParser(description='Flash FM-1 MIDI Processor firmware over USB MIDI.')
    ap.add_argument('firmware', nargs='?', default=str(DEFAULT_FIRMWARE), help='.fwsc package (default: the latest build)')
    ap.add_argument('--build', action='store_true', help='run src/build.bat before flashing')
    ap.add_argument('--info', action='store_true', help='show device identity and installed build, then exit')
    ap.add_argument('--list', action='store_true', help='list MIDI ports, then exit')
    ap.add_argument('--port', help='use the MIDI port whose name contains this text')
    ap.add_argument('--force', action='store_true', help='replace non-Open firmware (e.g. stock V15) or resume any loader')
    args = ap.parse_args()
    try:
        if args.list:
            mi, mo = rtmidi.MidiIn(), rtmidi.MidiOut()
            print('Inputs: ', mi.get_ports())
            print('Outputs:', mo.get_ports())
            return 0
        if args.info:
            info(args.port)
            return 0
        if args.build:
            result = subprocess.run(['cmd', '/c', str(SRC / 'build.bat')], cwd=SRC)
            check(result.returncode == 0, f'build.bat failed (exit {result.returncode}).')
        pkg = load_package(args.firmware)
        print(f'Package: {Path(args.firmware).name}  {pkg["product"]}  {pkg["bytes"]} bytes  '
              f'build {pkg["build_id"][:16]}')
        started = time.monotonic()
        flash(pkg, args.force, args.port)
        print(f'Finished in {time.monotonic() - started:.0f} s.')
        return 0
    except FlashError as e:
        print(f'\nError: {e}', file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print('\nInterrupted. If the device shows the loader, run the script again to resume.', file=sys.stderr)
        return 130


if __name__ == '__main__':
    sys.exit(main())
