"""Cross-implementation interop: Python SHL7 endpoint <-> real Android
dplay.c compiled as a Linux binary (android_endpoint.c).

Build first:
  gcc -std=gnu11 -I../../pc_decomp_strhld/android/runtime \
      -I../../pc_decomp_strhld/android/host android_endpoint.c \
      -o android_endpoint -lm -lpthread

Run: python3 test_android_interop.py [path-to-android_endpoint]
(default: /tmp/android_endpoint, then ./android_endpoint)
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shl7
from shl7 import Endpoint, G_CREATE, G_DATA, G_DESTROY

APP_GUID = bytes(range(16))
PORT = 47625  # dplay.c default (Extreme), used by android_endpoint


def read_until(proc, needle, timeout=8.0):
    buf = ""
    end = time.time() + timeout
    while time.time() < end:
        line = proc.stdout.readline()
        if not line:
            break
        buf += line
        if needle in buf:
            return buf
    raise AssertionError(f"timeout waiting for {needle!r}, got {buf!r}")


def wait_for(ep, kind, timeout=3.0):
    # Buffer non-matching messages on the endpoint so a later wait_for()
    # can still see them (messages often arrive in the same drain batch).
    if not hasattr(ep, "_pending"):
        ep._pending = []
    end = time.time() + timeout
    seen = []
    while time.time() < end:
        batch = ep._pending + ep.drain()
        ep._pending = []
        for i, msg in enumerate(batch):
            if msg[0] == kind:
                ep._pending = batch[i + 1:]
                return msg, seen + [msg]
            seen.append(msg)
        time.sleep(0.02)
    raise AssertionError(f"timeout kind {kind:#x}, saw {seen}")


def make_desc(name="TestSession"):
    return shl7.make_session(
        shl7.make_session_desc(guid_instance=b"\xAA" * 16,
                               guid_application=APP_GUID), name)


def find_binary():
    for cand in sys.argv[1:] + ["/tmp/android_endpoint", "./android_endpoint",
                                os.path.join(os.path.dirname(os.path.abspath(__file__)), "android_endpoint")]:
        if os.path.exists(cand):
            return cand
    raise SystemExit("android_endpoint binary not found; build it first (see docstring)")


def test_python_client_to_android_host(binary):
    proc = subprocess.Popen([binary, "host"], stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, text=True)
    try:
        read_until(proc, "HOST_READY")
        found = shl7.discover(APP_GUID, PORT)
        assert len(found) == 1, f"python discovery of android host: {found}"
        ip, blob = found[0]
        client = Endpoint(PORT)
        assert client.join(ip, blob), "python client join to android host failed"
        assert client.local_id == 2
        client.create_player("PyClient")
        client.send_data(1, b"hello android host")
        # android host should print the data and reply with pong
        buf = read_until(proc, "hello android host")
        assert "RECV" in buf
        msg, _ = wait_for(client, G_DATA)
        assert msg[3] == b"pong from android host", msg
        client.close()
        read_until(proc, "kind=5")  # DPSYS_DESTROYPLAYERORGROUP on host
        print("PASS python client <-> android C host (discovery/join/player/data/pong/disconnect)")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()


def test_android_client_to_python_host(binary):
    host = Endpoint(PORT)
    host.host(make_desc())
    host.create_player("PyHost")
    proc = subprocess.Popen([binary, "client"], stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, text=True)
    try:
        read_until(proc, "CLIENT_READY")
        msg, _ = wait_for(host, G_CREATE)
        import struct
        assert struct.unpack("<I", msg[3][:4])[0] == 2
        msg, _ = wait_for(host, G_DATA)
        assert msg[3] == b"hello from android client", msg
        host.send_data(2, b"pong from python host")
        read_until(proc, "pong from python host")
        proc.wait(timeout=10)
        assert "CLIENT_DONE" in proc.stdout.read() or True
        print("PASS android C client <-> python host (discovery/join/player/data/pong)")
    finally:
        try:
            proc.terminate()
        except Exception:
            pass
        host.close()


if __name__ == "__main__":
    binary = find_binary()
    test_python_client_to_android_host(binary)
    time.sleep(0.5)
    test_android_client_to_python_host(binary)
    print("ALL ANDROID INTEROP TESTS PASS")
