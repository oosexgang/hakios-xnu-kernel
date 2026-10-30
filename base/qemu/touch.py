#!/usr/bin/env python3
"""Touch the screen of the system running under QEMU (qemu/run-ios.sh), through QMP.

usage: touch.py STEP...
  tap X Y                     a short touch
  hold X Y MS                 touch and stay
  swipe X0 Y0 X1 Y1 [MS]      drag in MS milliseconds (default 300)
  key NAME [MS]               press a button: lock, home, volup, voldown; fivefingers = five fingers on the
                              screen for MS (held 1500 ms they end the run)
  stacks PROCESS              have the kernel print where the threads of that process are (the
                              addresses come out in the log; qemu/symstack.py names them).
                              "stacks top" is something else: which processes have had the processor
                              since it was last asked ("top:" lines in the log)
  wait SECONDS
Positions are fractions of the screen: 0 0 is the top left corner, 1 1 the bottom right.
Example, the "swipe up to open" gesture:  touch.py swipe 0.5 0.98 0.5 0.4 250
"""
import json
import os
import socket
import sys
import time


def instance():
    """Which QEMU: QEMU_INSTANCE, or the number in the file "instance" next to this script, or 0."""
    try:
        return int(os.environ.get('QEMU_INSTANCE') or
                   open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'instance')).read())
    except (OSError, ValueError):
        return 0

KEYS = {'lock': 'l', 'home': 'h', 'volup': 'up', 'voldown': 'down', 'fivefingers': 'f11'}


class Qmp:
    def __init__(self, port=None):
        self.sock = socket.create_connection(('127.0.0.1', port or 4445 + 10 * instance()))
        self.file = self.sock.makefile('rw')
        self.file.readline()                        # the greeting
        self.command('qmp_capabilities')

    def command(self, name, **arguments):
        self.file.write(json.dumps({'execute': name, 'arguments': arguments}) + '\n')
        self.file.flush()
        while True:
            reply = json.loads(self.file.readline())
            if 'return' in reply or 'error' in reply:
                if 'error' in reply:
                    sys.exit(f'touch.py: {name}: {reply["error"]["desc"]}')
                return reply['return']

    def events(self, *events):
        self.command('input-send-event', events=list(events))

    def pointer(self, x, y, down=None):
        events = [{'type': 'abs', 'data': {'axis': 'x', 'value': round(x * 0x7fff)}},
                  {'type': 'abs', 'data': {'axis': 'y', 'value': round(y * 0x7fff)}}]
        if down is not None:
            events.append({'type': 'btn', 'data': {'button': 'left', 'down': down}})
        self.events(*events)

    def key(self, name, down):
        self.events({'type': 'key', 'data': {'key': {'type': 'qcode', 'data': name}, 'down': down}})


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    qmp = Qmp()
    while args:
        step = args.pop(0)
        if step == 'wait':
            time.sleep(float(args.pop(0)))
        elif step in ('tap', 'hold'):
            x, y = float(args.pop(0)), float(args.pop(0))
            ms = float(args.pop(0)) if step == 'hold' else 60
            qmp.pointer(x, y)
            qmp.pointer(x, y, True)
            time.sleep(ms / 1000)
            qmp.pointer(x, y, False)
        elif step == 'swipe':
            x0, y0, x1, y1 = (float(args.pop(0)) for _ in range(4))
            ms = float(args.pop(0)) if args and args[0][0].isdigit() else 300
            steps = max(2, int(ms / 16))
            qmp.pointer(x0, y0)
            qmp.pointer(x0, y0, True)
            for i in range(1, steps + 1):
                time.sleep(ms / 1000 / steps)
                qmp.pointer(x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps)
            time.sleep(0.016)
            qmp.pointer(x1, y1, False)
        elif step == 'stacks':
            # F12, the name, Enter: the kernel's virtio keyboard driver takes it as a command
            special = {'.': 'dot', '-': 'minus', '_': 'minus', ' ': 'spc'}
            qmp.key('f12', True)
            qmp.key('f12', False)
            for ch in args.pop(0):
                shifted = ch.isupper() or ch == '_'
                code = special.get(ch, ch.lower())
                if shifted:
                    qmp.key('shift', True)
                qmp.key(code, True)
                qmp.key(code, False)
                if shifted:
                    qmp.key('shift', False)
                time.sleep(0.02)
            qmp.key('ret', True)
            qmp.key('ret', False)
        elif step == 'key':
            name = KEYS.get(args.pop(0))
            if name is None:
                sys.exit('touch.py: buttons are: ' + ' '.join(KEYS))
            ms = float(args.pop(0)) if args and args[0][0].isdigit() else 80
            qmp.key(name, True)
            time.sleep(ms / 1000)
            qmp.key(name, False)
        else:
            sys.exit(f'touch.py: bad step "{step}"')
        time.sleep(0.05)


if __name__ == '__main__':
    main()
