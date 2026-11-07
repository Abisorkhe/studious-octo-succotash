"""Interop: real C++ SHL7 provider (compiled for Linux via
tests/linux_shim) <-> Android C dplay.c endpoint + Python endpoint.

Build:
  g++ -std=c++17 -I tests/linux_shim -I windows windows/Shl7PlayProvider.cpp \
      tests/provider_linux_test.cpp -o provider_linux -pthread
  gcc -std=gnu11 -I../../pc_decomp_strhld/android/runtime \
      -I../../pc_decomp_strhld/android/host tests/android_endpoint.c \
      -o android_endpoint -lm -lpthread

Run: python3 test_provider_linux.py [provider_linux] [android_endpoint]
"""
import os
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shl7
from shl7 import Endpoint, G_CREATE, G_DATA, G_DESTROY

APP_GUID = bytes(range(16))
PORT = 47625


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


def find(args, default_names):
    for cand in list(args) + default_names:
        if cand and os.path.exists(cand):
            return cand
    return None


def test_provider_host_vs_android_client(provider_bin, android_bin):
    proc = subprocess.Popen([provider_bin, "host"], stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, text=True)
    try:
        read_until(proc, "WHOST_READY")
        proc2 = subprocess.Popen([android_bin, "client"], stdout=subprocess.PIPE,
                                 stderr=subprocess.DEVNULL, text=True)
        try:
            read_until(proc2, "WCLIENT_READY") if False else read_until(proc2, "CLIENT_READY")
            read_until(proc, "hello from android client")
            read_until(proc2, "pong from windows provider")
            print("PASS windows provider (C++) host <-> android C client")
        finally:
            proc2.terminate()
    finally:
        proc.terminate()


def test_android_host_vs_provider_client(provider_bin, android_bin):
    proc = subprocess.Popen([android_bin, "host"], stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, text=True)
    try:
        read_until(proc, "HOST_READY")
        proc2 = subprocess.Popen([provider_bin, "client"], stdout=subprocess.PIPE,
                                 stderr=subprocess.DEVNULL, text=True)
        try:
            read_until(proc2, "WCLIENT_READY")
            read_until(proc, "hello from windows provider")
            read_until(proc2, "pong from android host")
            print("PASS android C host <-> windows provider (C++) client")
        finally:
            proc2.terminate()
    finally:
        proc.terminate()


def test_provider_host_vs_python_client(provider_bin):
    proc = subprocess.Popen([provider_bin, "host"], stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, text=True)
    try:
        read_until(proc, "WHOST_READY")
        found = shl7.discover(APP_GUID, PORT)
        assert len(found) == 1, f"discovery of provider host: {found}"
        ip, blob = found[0]
        client = Endpoint(PORT)
        assert client.join(ip, blob)
        assert client.local_id == 2
        client.create_player("PyClient")
        client.send_data(1, b"hello windows provider")
        read_until(proc, "hello windows provider")
        msg, _ = wait_for(client, G_DATA)
        assert msg[3] == b"pong from windows provider", msg
        client.close()
        read_until(proc, "kind=5")
        print("PASS windows provider (C++) host <-> python client")
    finally:
        proc.terminate()


if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    provider_bin = sys.argv[1] if len(sys.argv) > 1 else find([], ["/tmp/provider_linux", os.path.join(here, "provider_linux")])
    android_bin = sys.argv[2] if len(sys.argv) > 2 else find([], ["/tmp/android_endpoint", os.path.join(here, "android_endpoint")])
    if not provider_bin:
        raise SystemExit("provider_linux binary not found; build it first (see docstring)")
    test_provider_host_vs_python_client(provider_bin)
    time.sleep(0.5)
    if android_bin:
        test_provider_host_vs_android_client(provider_bin, android_bin)
        time.sleep(0.5)
        test_android_host_vs_provider_client(provider_bin, android_bin)
    print("ALL PROVIDER LINUX TESTS PASS")
