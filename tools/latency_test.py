#!/usr/bin/env python3
"""Measure MIDI round-trip latency through the FM-1 over USB.

Sends Note On messages to the FM-1's USB input and times how long each takes
to come back on its USB output. The result includes the host's own USB MIDI
stack in both directions, so compare runs against each other rather than
reading the number as the device's processing time alone.

    python tools/latency_test.py                 200 notes, one at a time
    python tools/latency_test.py --count 1000
    python tools/latency_test.py --burst 8       8 notes sent back to back per probe

After the run it also prints the device's own figures (firmware with the
latency diagnostics): time from a message arriving at the FM-1 to the USB
transport accepting the processed output, which excludes the host.

Close other programs using the port. Needs python-rtmidi.
"""
import argparse
import queue
import statistics
import sys
import time

import rtmidi


def open_port(cls, name):
    port = cls()
    names = port.get_ports()
    index = next((i for i, p in enumerate(names) if name.lower() in p.lower()), None)
    if index is None:
        sys.exit(f'No port matching "{name}": {names}')
    port.open_port(index)
    return port, names[index]


def percentile(values, p):
    ordered = sorted(values)
    k = (len(ordered) - 1) * p / 100.0
    lo = int(k)
    hi = min(lo + 1, len(ordered) - 1)
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (k - lo)


DIAG_READ = [0xF0, 0x7D, 0x4D, 0x50, 0x01, 0xF7]
DIAG_RESET = [0xF0, 0x7D, 0x4D, 0x50, 0x02, 0xF7]
DIAG_FIELDS = ['count', 'sum_us', 'max_us', 'retries', 'screen_max_us', 'service_max_us',
               'wake_max_us', 'slow_idle', 'slow_busy', 'polls_max_us', 'wake_fix', 'lost_input',
               'lost_output']
DIAG_BUCKETS = ['<50us', '<100us', '<200us', '<500us', '<1ms', '<2ms', '<5ms', '>=5ms']


def read_device_stats(out, arrivals):
    """Ask the FM-1 for its latency counters; None if it doesn't answer."""
    while not arrivals.empty():
        arrivals.get_nowait()
    out.send_message(DIAG_READ)
    deadline = time.perf_counter() + 0.5
    while time.perf_counter() < deadline:
        try:
            _, msg = arrivals.get(timeout=0.1)
        except queue.Empty:
            continue
        if msg[:5] == DIAG_READ[:5] and msg[-1] == 0xF7:
            body = msg[5:-1]
            values = [sum(body[i + k] << (7 * k) for k in range(5)) & 0xFFFFFFFF
                      for i in range(0, len(body) - 4, 5)]
            names = DIAG_FIELDS + DIAG_BUCKETS + ['iterations']
            return dict(zip(names, values))
    return None


def print_device_stats(d):
    if not d:
        print('\nDevice stats: no reply (firmware without latency diagnostics?)')
        return
    print(f"\nOn the device (arrival -> USB transport), {d['count']} messages:")
    if d['count']:
        print(f"  mean {d['sum_us'] / d['count']:.0f} us   max {d['max_us']} us")
        print('  ' + '  '.join(f'{b} {d[b]}' for b in DIAG_BUCKETS))
    print(f"  longest wait before processing {d['wake_max_us']} us; waits over 2 ms: "
          f"{d['slow_idle']} with the control task idle, {d['slow_busy']} while it was busy")
    print(f"  longest: MIDI service {d['service_max_us']} us, screen update {d['screen_max_us']} us, "
          f"library polls {d['polls_max_us']} us")
    print(f"  wake-up fix {'active' if d['wake_fix'] else 'NOT active (library code not recognised)'}")
    print(f"  transport-full retries {d['retries']}   lost in {d['lost_input']}   lost out {d['lost_output']}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--count', type=int, default=200, help='number of probes')
    ap.add_argument('--burst', type=int, default=1, help='notes per probe, sent back to back')
    ap.add_argument('--gap', type=float, default=0.01, help='seconds between probes')
    ap.add_argument('--timeout', type=float, default=0.25, help='seconds to wait for an echo')
    ap.add_argument('--port', default='USB-Midi', help='port name (substring)')
    ap.add_argument('--channel', type=int, default=16, help='MIDI channel for probes, 1-16')
    ap.add_argument('--idle', action='store_true',
                    help='only measure how often the control loop runs with no MIDI (2 s)')
    args = ap.parse_args()

    out, out_name = open_port(rtmidi.MidiOut, args.port)
    inp, in_name = open_port(rtmidi.MidiIn, args.port)
    inp.ignore_types(sysex=False, timing=True, active_sense=True)
    arrivals = queue.Queue()
    inp.set_callback(lambda event, _: arrivals.put((time.perf_counter(), event[0])))
    print(f'Out: {out_name}\nIn:  {in_name}')

    if args.idle:
        try:
            time.sleep(0.2)
            out.send_message(DIAG_RESET)
            time.sleep(2.0)
            d = read_device_stats(out, arrivals)
        finally:
            inp.close_port()
            out.close_port()
        if not d or 'iterations' not in d:
            sys.exit('No reply (firmware without the iteration counter?)')
        rate = d['iterations'] / 2.0
        print(f'Control loop with no MIDI: {rate:.0f} iterations/s, '
              f'so panel input waits up to {1000 / rate:.1f} ms' if rate else 'Control loop not running')
        return

    on = 0x90 | (args.channel - 1)
    off = 0x80 | (args.channel - 1)
    first, last, lost = [], [], 0
    try:
        time.sleep(0.2)
        while not arrivals.empty():
            arrivals.get_nowait()
        out.send_message(DIAG_RESET)
        time.sleep(0.05)
        for i in range(args.count):
            notes = [(i * args.burst + b) % 128 for b in range(args.burst)]
            velocity = 1 + i % 127
            pending = {n: None for n in notes}
            sent = time.perf_counter()
            for n in notes:
                out.send_message([on, n, velocity])
            deadline = sent + args.timeout
            while any(v is None for v in pending.values()):
                remaining = deadline - time.perf_counter()
                if remaining <= 0:
                    break
                try:
                    t, msg = arrivals.get(timeout=remaining)
                except queue.Empty:
                    break
                if len(msg) == 3 and msg[0] == on and msg[2] == velocity and msg[1] in pending:
                    pending[msg[1]] = t
            got = [t for t in pending.values() if t is not None]
            lost += len(notes) - len(got)
            if got:
                first.append((min(got) - sent) * 1000)
                if len(got) == len(notes):
                    last.append((max(got) - sent) * 1000)
            for n in notes:
                out.send_message([off, n, 0])
            time.sleep(args.gap)
            while not arrivals.empty():
                arrivals.get_nowait()
        device = read_device_stats(out, arrivals)
    finally:
        inp.close_port()
        out.close_port()

    if not first:
        sys.exit('No echoes received. Is the FM-1 passing USB input back to USB output?')

    def report(label, values):
        print(f'{label:<22} min {min(values):6.2f}  median {statistics.median(values):6.2f}  '
              f'p95 {percentile(values, 95):6.2f}  p99 {percentile(values, 99):6.2f}  '
              f'max {max(values):6.2f} ms')

    print(f'\n{args.count} probes of {args.burst} note(s), {lost} lost')
    report('first note round trip', first)
    if args.burst > 1 and last:
        report('whole burst', last)
    print_device_stats(device)


if __name__ == '__main__':
    main()
