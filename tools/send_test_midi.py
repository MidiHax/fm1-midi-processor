#!/usr/bin/env python3
"""Send one of each MIDI message type to the FM-1 over USB, for checking the
MIDI Monitor screen. Needs python-rtmidi. Close other programs using the port.

    python tools/send_test_midi.py            one pass of every type
    python tools/send_test_midi.py --clock    also 4 beats of MIDI clock at 120 BPM
    python tools/send_test_midi.py --flood N  N extra Note On/Off pairs (scroll test)
"""
import argparse
import sys
import time

import rtmidi

# (message, what the monitor should show)
MESSAGES = [
    ([0x90, 60, 100], 'U 01 NON C4   100'),
    ([0x80, 60, 0], 'U 01 NOF C4   0'),
    ([0x99, 1, 127], 'U 10 NON C#-1 127'),
    ([0x9F, 127, 1], 'U 16 NON G9   1'),
    ([0xA0, 64, 50], 'U 01 PAT E4   50'),
    ([0xB1, 7, 64], 'U 02 CC  7    64'),
    ([0xC2, 12], 'U 03 PC  12'),
    ([0xD3, 45], 'U 04 CAT 45'),
    ([0xE0, 0x00, 0x40], 'U 01 PB  +0'),
    ([0xE0, 0x00, 0x3C], 'U 01 PB  -512'),
    ([0xE0, 0x7F, 0x7F], 'U 01 PB  +8191'),
    ([0xF2, 0x10, 0x01], 'U -- SPP 144'),
    ([0xF3, 5], 'U -- SS  5'),
    ([0xF6], 'U -- TR'),
    ([0xF0, 0x7D, 0x01, 0x02, 0x03, 0xF7], 'U -- SYX 6B'),
    ([0xFE], '(Active Sensing: not shown)'),
    ([0xFF], 'U -- RST'),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--clock', action='store_true', help='send Start, 4 beats of clock, Stop')
    ap.add_argument('--flood', type=int, default=0, help='extra note pairs to fill the buffer')
    ap.add_argument('--port', default='USB-Midi', help='output port name (substring)')
    args = ap.parse_args()

    out = rtmidi.MidiOut()
    ports = out.get_ports()
    index = next((i for i, p in enumerate(ports) if args.port.lower() in p.lower()), None)
    if index is None:
        sys.exit(f'No output port matching "{args.port}": {ports}')
    out.open_port(index)
    print(f'Sending to {ports[index]}. Expected on screen, oldest first:')
    try:
        for message, shown in MESSAGES:
            out.send_message(message)
            print(f'  {shown}')
            time.sleep(0.05)
        if args.clock:
            out.send_message([0xFA])
            print('  U -- STA       then CLK blinks in the header for 2 s')
            for _ in range(4 * 24):
                out.send_message([0xF8])
                time.sleep(0.5 / 24)
            out.send_message([0xFC])
            print('  U -- STP')
        for i in range(args.flood):
            note = 48 + i % 24
            out.send_message([0x90, note, 90])
            out.send_message([0x80, note, 0])
            time.sleep(0.005)
        if args.flood:
            print(f'  ...then {2 * args.flood} note messages')
    finally:
        out.close_port()


if __name__ == '__main__':
    main()
