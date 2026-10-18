#!/usr/bin/env python3
"""xts_tftp.py - serve ONE file to XeLL's TFTP boot: "xenon.elf" -> an XTS ELF. Read-only.

    make serve PROFILE=smoke          # builds the ELF, sets up the venv, runs this via sudo (port 69)
    sudo build/venv/bin/python tools/xts_tftp.py --elf build/xts-smoke.elf32 [--port 69] [--host 0.0.0.0]
    build/venv/bin/python tools/xts_tftp.py --selftest

Safety: XeLL's TFTP loop also asks for NAND and XeLL update images by name, and tftpy's server would by default
accept uploads. This server's root is an empty directory, so no request ever matches a real file; the only name that
is answered is --name (default "xenon.elf"); every other name gets "file not found"; every upload is refused.
The ELF is opened for every request, so a rebuild is picked up without restarting.

Point XeLL at this PC: put a file kboot.conf containing the line  tftp_server=<this PC's IPv4 address>  on the USB
stick (and no xenon.elf on the stick, or XeLL boots that instead). Without it XeLL tries the DHCP server's address.
"""
import argparse
import io
import logging
import os
import socket
import sys
import tempfile
import threading
import time

try:
    import tftpy
except ImportError:
    sys.exit("xts_tftp: the tftpy library is missing - use 'make serve' (it creates build/venv), or "
             "pip install -r tools/requirements.txt in a virtual environment")


def make_server(root, elf, name):
    want = name.lstrip("/")

    def dyn_file(path, raddress=None, rport=None, **_):
        req = str(path).replace("\\", "/").lstrip("/")
        if req != want:
            print("  %s: refused %r (only %r is served)" % (raddress, req, want), flush=True)
            return None                                     # tftpy answers "file not found"
        # opened per request, so a rebuilt ELF is picked up without restarting. A real file object: tftpy 0.8.7 locks
        # (fcntl) the object of every session regardless of the server's flock setting.
        print("  %s: sending %s (%d bytes, %s)" % (raddress, want, os.path.getsize(elf), os.path.basename(elf)), flush=True)
        return open(elf, "rb")

    def no_upload(path, context):
        print("  upload refused: %r" % path, flush=True)
        return None                                         # tftpy answers "access violation"

    return tftpy.TftpServer(root, dyn_file_func=dyn_file, upload_open=no_upload)


def serve(elf, host, port, name):
    if not os.path.isfile(elf):
        sys.exit("xts_tftp: no such file: %s (build it first: make PROFILE=...)" % elf)
    logging.getLogger("tftpy").setLevel(logging.WARNING)
    root = tempfile.mkdtemp(prefix="xts-tftp-empty-")       # empty: nothing on disk can ever be served
    srv = make_server(root, elf, name)
    print("xts_tftp: serving %s as %s on %s:%d (Ctrl-C to stop)" % (elf, name, host, port), flush=True)
    try:
        srv.listen(host, port)
    except PermissionError:
        sys.exit("xts_tftp: port %d needs root - run it with sudo (make serve does)" % port)
    except KeyboardInterrupt:
        pass
    finally:
        os.rmdir(root)


def selftest():
    """Serve a fake ELF on an ephemeral port, then: fetch xenon.elf (must match), fetch other names (must fail),
    upload (must be refused and nothing written)."""
    logging.getLogger("tftpy").setLevel(logging.CRITICAL)
    work = tempfile.mkdtemp(prefix="xts-tftp-test-")
    elf = os.path.join(work, "x.elf32")
    payload = bytes((i * 7 + 3) & 255 for i in range(70000))   # > 128 blocks of 512
    with open(elf, "wb") as f:
        f.write(payload)
    root = tempfile.mkdtemp(prefix="xts-tftp-empty-")
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    srv = make_server(root, elf, "xenon.elf")
    t = threading.Thread(target=srv.listen, args=("127.0.0.1", port), daemon=True)
    t.start()
    time.sleep(0.3)
    ok = True

    def fetch(name):
        out = io.BytesIO()
        try:
            tftpy.TftpClient("127.0.0.1", port).download(name, out, timeout=2)
            return out.getvalue()
        except Exception:
            return None

    got = fetch("xenon.elf")
    if got != payload:
        print("FAIL: xenon.elf content mismatch (%s)" % (None if got is None else len(got)))
        ok = False
    for bad in ("updflash.bin", "updxell.bin", "kboot.conf", "../x.elf32", "/etc/passwd", "XENON.ELF"):
        if fetch(bad) is not None:
            print("FAIL: %r was served" % bad)
            ok = False
    try:
        tftpy.TftpClient("127.0.0.1", port).upload("xenon.elf", io.BytesIO(b"evil"), timeout=2)
        print("FAIL: upload accepted")
        ok = False
    except Exception:
        pass
    if fetch("xenon.elf") != payload:                      # the server must still be alive after the refusals
        print("FAIL: server stopped answering after the refused requests")
        ok = False
    if not t.is_alive():
        print("FAIL: server thread died")
        ok = False
    if os.listdir(root) or open(elf, "rb").read() != payload:
        print("FAIL: something was written")
        ok = False
    srv.stop(now=True)
    print("xts_tftp selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description="serve one XTS ELF to XeLL's TFTP boot (read-only)")
    ap.add_argument("--elf", help="the ELF to serve (e.g. build/xts-smoke.elf32)")
    ap.add_argument("--name", default="xenon.elf", help="the only file name answered (default xenon.elf)")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=69)
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.elf:
        ap.error("--elf is required")
    serve(a.elf, a.host, a.port, a.name)
    return 0


if __name__ == "__main__":
    sys.exit(main())
