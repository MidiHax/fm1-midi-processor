#!/usr/bin/env python3
"""Back up, restore and inspect FM-1 MIDI Processor patches over SysEx.

Format: design/patch-format.md. Messages are F0 7D <device> 11 <command> ... F7.

  python tools/patch_sysex.py get edit              print the patch in use (edit buffer)
  python tools/patch_sysex.py get 3 -o p3.syx       save stored patch 3 (1-16) as a .syx file
  python tools/patch_sysex.py get all -o all.syx    save all 16 patches
  python tools/patch_sysex.py put p3.syx            send a .syx file back (it names its own slots)
  python tools/patch_sysex.py put p3.syx --to edit  send it to the edit buffer instead (no Flash write)
  python tools/patch_sysex.py put p3.syx --to 5     send it to patch 5

Requests need ALLOW DUMP REQ = YES on the Sysex Dump page, and --device must
match its DEVICE ID. Close other programs using the port. Needs python-rtmidi.
"""
import argparse
import sys
import time

import rtmidi

MANUFACTURER, MODEL = 0x7D, 0x11
DUMP, DUMP_ALL, REQUEST, REQUEST_ALL = 0x00, 0x01, 0x10, 0x11
EDIT = 0x7F
BODY_PER_PACKET = 98


def open_port(cls, name):
    port = cls()
    names = port.get_ports()
    index = next((i for i, p in enumerate(names) if name.lower() in p.lower()), None)
    if index is None:
        sys.exit(f'No port matching "{name}": {names}')
    port.open_port(index)
    return port, names[index]


def pack(data):
    out = []
    for g in range(0, len(data), 7):
        group = data[g:g + 7]
        out.append(sum(((b >> 7) & 1) << i for i, b in enumerate(group)))
        out.extend(b & 0x7F for b in group)
    return out


def unpack(data):
    out = []
    for g in range(0, len(data), 8):
        top, group = data[g], data[g + 1:g + 8]
        out.extend(b | (((top >> i) & 1) << 7) for i, b in enumerate(group))
    return out


def checksum(data):
    return (128 - sum(data) % 128) % 128


def packets(device, command, slot, body):
    count = max(1, -(-len(body) // BODY_PER_PACKET))
    for i in range(count):
        d = [slot, i, count] + pack(body[i * BODY_PER_PACKET:(i + 1) * BODY_PER_PACKET])
        yield [0xF0, MANUFACTURER, device, MODEL, command] + d + [checksum(d), 0xF7]


def parse(messages, device):
    """Reassemble dump packets into {slot: body}. Raises on a bad packet."""
    patches, partial = {}, {}
    for m in messages:
        if len(m) < 10 or m[:4] != [0xF0, MANUFACTURER, device, MODEL] or m[4] not in (DUMP, DUMP_ALL):
            continue
        d = m[5:-1]
        if checksum(d[:-1]) != d[-1]:
            raise ValueError(f'checksum error in a packet for slot {d[0]}')
        slot, index, count = d[0], d[1], d[2]
        chunks = partial.setdefault(slot, [])
        if index != len(chunks):
            raise ValueError(f'slot {slot}: packet {index} out of order')
        chunks.append(unpack(d[3:-1]))
        if index + 1 == count:
            patches[slot] = [b for c in partial.pop(slot) for b in c]
    return patches


def describe(slot, body):
    where = 'EDIT' if slot == EDIT else f'{slot + 1:2}'
    name = bytes(body[1:13]).decode('ascii', 'replace')
    ch, msg = body[88], body[89] | body[90] << 8
    text = f'patch {where}  "{name}"  v{body[0]}  {len(body)} bytes  {ch} channel rules  {msg} message rules'
    end = 91 + 3 * ch + 6 * msg
    if body[0] >= 2 and len(body) >= end + 3:  # version 2: event triggers
        text += f'  {body[end]} triggers  {body[end + 1] | body[end + 2] << 8} trigger messages'
    return text


def slot_arg(text):
    if text.lower() == 'edit':
        return EDIT
    if text.lower() == 'all':
        return 'all'
    n = int(text)
    if not 1 <= n <= 16:
        raise argparse.ArgumentTypeError('patch numbers are 1-16')
    return n - 1


def receive(inp, quiet=0.5, limit=10.0):
    """Collect SysEx until `quiet` seconds pass with nothing new."""
    messages, start, last = [], time.time(), time.time()
    while time.time() - start < limit:
        msg = inp.get_message()
        if msg:
            if msg[0][0] == 0xF0:
                messages.append(msg[0])
            last = time.time()
        elif time.time() - last > quiet:
            break
        else:
            time.sleep(0.002)
    return messages


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', default='USB-Midi', help='port name (substring)')
    ap.add_argument('--device', type=int, default=0, help='DEVICE ID on the Sysex Dump page (0-127)')
    sub = ap.add_subparsers(dest='action', required=True)
    g = sub.add_parser('get', help='request patches')
    g.add_argument('what', type=slot_arg, help='1-16, all or edit')
    g.add_argument('-o', '--output', help='save the dump as a .syx file')
    p = sub.add_parser('put', help='send a .syx file')
    p.add_argument('file')
    p.add_argument('--to', type=slot_arg, help='send a single patch to 1-16 or edit instead')
    p.add_argument('--gap', type=float, default=0.005, help='seconds between packets')
    args = ap.parse_args()

    out, out_name = open_port(rtmidi.MidiOut, args.port)
    inp, in_name = open_port(rtmidi.MidiIn, args.port)
    inp.ignore_types(sysex=False, timing=True, active_sense=True)
    print(f'Out: {out_name}\nIn:  {in_name}')
    # Windows' MIDI input isn't delivering yet just after the port opens: a
    # reply to a message sent at once (about 1 ms later) is sometimes dropped
    # by the host. The FM-1 itself receives and answers it (checked with its
    # counters). Wait before the first message.
    time.sleep(0.05)

    if args.action == 'get':
        while inp.get_message():
            pass
        if args.what == 'all':
            out.send_message([0xF0, MANUFACTURER, args.device, MODEL, REQUEST_ALL, 0xF7])
        else:
            out.send_message([0xF0, MANUFACTURER, args.device, MODEL, REQUEST, args.what, 0xF7])
        messages = receive(inp)
        patches = parse(messages, args.device)
        if not patches:
            sys.exit('No reply. Check ALLOW DUMP REQ and DEVICE ID on the Sysex Dump page.')
        for slot in sorted(patches):
            print(describe(slot, patches[slot]))
        if args.output:
            with open(args.output, 'wb') as f:
                for m in messages:
                    f.write(bytes(m))
            print(f'{len(messages)} packets saved to {args.output}')
        return

    data = open(args.file, 'rb').read()
    messages, start = [], None
    for i, b in enumerate(data):
        if b == 0xF0:
            start = i
        elif b == 0xF7 and start is not None:
            messages.append(list(data[start:i + 1]))
            start = None
    patches = parse(messages, messages[0][2] if messages else args.device)
    if not patches:
        sys.exit('No patch dump in the file.')
    if args.to is not None:
        if len(patches) != 1:
            sys.exit('--to needs a file holding a single patch.')
        patches = {args.to: next(iter(patches.values()))}
    command = DUMP_ALL if len(patches) == 16 else DUMP
    sent = 0
    for slot in sorted(patches):
        print('send ' + describe(slot, patches[slot]))
        for m in packets(args.device, command, slot, patches[slot]):
            out.send_message(m)
            sent += 1
            time.sleep(args.gap)
    print(f'{sent} packets sent. Patches for slots are written to Flash 0.1 s after the last packet.')


if __name__ == '__main__':
    main()
