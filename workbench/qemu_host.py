#!/usr/bin/env python3
"""What the machine in QEMU reaches on this computer, through QEMU's gateway (10.0.2.2 there is
127.0.0.1 here), for workbench/qemu_boot_test.sh: a directory served over HTTP, and an echo of the
first datagram that comes on a UDP port, answered with "echoed: " before it.

    workbench/qemu_host.py DIRECTORY HTTP_PORT UDP_PORT SECONDS

Both stop after SECONDS.
"""
import functools
import http.server
import socket
import sys
import threading


class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):       # (the test reads the machine's side, not this one's)
        pass


def main(argv):
    if len(argv) != 5:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    directory, http_port, udp_port, seconds = argv[1], int(argv[2]), int(argv[3]), float(argv[4])
    httpd = http.server.HTTPServer(('127.0.0.1', http_port), functools.partial(Quiet, directory=directory))
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
        udp.bind(('127.0.0.1', udp_port))
        udp.settimeout(seconds)
        try:
            data, peer = udp.recvfrom(2048)
            udp.sendto(b'echoed: ' + data, peer)
        except socket.timeout:
            pass
    threading.Event().wait(5)           # (the page may still be on its way)
    httpd.shutdown()
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
