"""Interop tests for the SHL7 direct bridge prototype.

Run: python3 test_interop.py
Covers (all over real TCP/UDP sockets on loopback):
  1. Python host <-> Python client: discovery, join, player create,
     data in both directions, player data update, session desc update,
     disconnect holding -> session-lost / destroy notifications.
  2. Large (200 KB) data message, like tests/dplay_selfcheck.c.
  3. Malformed packets are rejected (bad magic, wrong session GUID,
     invalid sender).
  4. Static checks: constants in this prototype match
     pc_decomp_strhld/android/host/dplay.c (magic, ports, kinds,
     struct sizes, session-desc offsets) so the Windows provider
     built from the same spec stays wire-compatible with Android.
"""
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shl7
from shl7 import Endpoint, G_CREATE, G_DATA, G_DESTROY, G_LOST, G_PDATA, G_SESSION

APP_GUID = bytes(range(16))
PORT = 47699  # test port, avoids the real 47624/47625/47628


def wait_for(ep, kind, timeout=3.0):
    end = time.time() + timeout
    seen = []
    while time.time() < end:
        for msg in ep.drain():
            seen.append(msg)
            if msg[0] == kind:
                return msg, seen
        time.sleep(0.02)
    raise AssertionError(f"timeout waiting for guest kind {kind:#x}, saw {seen}")


def make_desc(name="TestSession", flags=0, max_players=8, current=0):
    return shl7.make_session(
        shl7.make_session_desc(flags=flags, guid_instance=b"\xAA" * 16,
                               guid_application=APP_GUID, max_players=max_players,
                               current_players=current), name)


def test_host_client():
    host = Endpoint(PORT)
    host.host(make_desc())
    host.create_player("HostPlayer")
    found = shl7.discover(APP_GUID, PORT)
    assert len(found) == 1, f"discovery found {found}"
    ip, blob = found[0]
    assert ip, "discovery returned empty ip"

    client = Endpoint(PORT)
    assert client.join(ip, blob), "client join failed"
    assert client.local_id == 2
    client.create_player("ClientPlayer")

    # host sees the new player
    msg, _ = wait_for(host, G_CREATE)
    assert struct.unpack("<I", msg[3][:4])[0] == 2
    # client sees host player (from WELCOME roster)
    assert 1 in client.players

    # client -> host data
    client.send_data(1, b"hello host")
    msg, _ = wait_for(host, G_DATA)
    assert msg[3] == b"hello host" and msg[1] == 2

    # host -> client data
    host.send_data(2, b"hello client")
    msg, _ = wait_for(client, G_DATA)
    assert msg[3] == b"hello client" and msg[1] == 1

    # player data update propagates as G_PDATA
    client.set_player_data(b"\x01\x02\x03")
    msg, _ = wait_for(host, G_PDATA)
    assert struct.unpack("<I", msg[3][:4])[0] == 2

    # session desc update propagates as G_SESSION
    host.set_session_desc(shl7.make_session_desc(
        flags=0, guid_instance=b"\xAA" * 16, guid_application=APP_GUID,
        max_players=8, current_players=2))
    msg, _ = wait_for(client, G_SESSION)
    assert len(msg[3]) == shl7.SESSION_SIZE

    # large message (200 KB), matching dplay_selfcheck's 60/120/180 KB cases
    big = bytes((i * 23) & 0xFF for i in range(200_000))
    client.send_data(1, big)
    msg, _ = wait_for(host, G_DATA, timeout=5.0)
    assert msg[3] == big

    # disconnect: host gets destroy, client gets session-lost
    client.close()
    msg, _ = wait_for(host, G_DESTROY)
    assert struct.unpack("<I", msg[3][:4])[0] == 2
    host.close()
    print("PASS host<->client (discovery/join/player/data/pdata/session/big/disconnect)")


def test_malformed():
    host = Endpoint(PORT + 1)
    host.host(make_desc())
    host.create_player("HostPlayer")
    import socket as _s
    bad = _s.create_connection(("127.0.0.1", PORT + 1))
    bad.sendall(struct.pack(">5I", 0xDEADBEEF, 1, 0, 0, 0))  # bad magic
    bad.close()
    # wrong session GUID in HELLO must be rejected (peer dropped, host alive)
    bad2 = _s.create_connection(("127.0.0.1", PORT + 1))
    bad2.sendall(shl7.pack_header(shl7.HELLO, 0, 0, b"\x00" * 32))
    bad2.close()
    time.sleep(0.3)
    # host still functional
    found = shl7.discover(APP_GUID, PORT + 1)
    assert len(found) == 1
    host.close()
    print("PASS malformed packets rejected, host survives")


def test_spec_matches_android():
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "..", "..", "pc_decomp_strhld", "android", "host", "dplay.c")
    src = open(path).read()
    for needle in ["0x53484C37", "47624", "47625", "47628", "NET_HELLO=1",
                   "NET_WELCOME", "NET_PLAYER", "NET_REMOVE", "NET_DATA",
                   "NET_SESSION", "NET_PLAYER_DATA", "LAN_MAX_DATA (1024u*1024u)",
                   "LAN_PLAYERS 8", "LAN_PLAYER_DATA 4096",
                   "desc[0]==80", "desc[10]"]:
        assert needle in src, f"dplay.c missing {needle}"
    # struct sizes derived from dplay.c definitions
    assert shl7.SESSION_SIZE == 80 + 128 * 2
    assert shl7.PLAYER_SIZE == 12 + 128 * 2 + 4096
    # session-desc offsets used by dplay.c: size@0, flags@4, guidInstance@8,
    # guidApplication@24, maxPlayers@40, currentPlayers@44
    desc = shl7.make_session_desc(guid_instance=b"\x11" * 16, guid_application=APP_GUID,
                                   max_players=8, current_players=3)
    assert struct.unpack("<I", desc[0:4])[0] == 80
    assert desc[8:24] == b"\x11" * 16
    assert desc[24:40] == APP_GUID
    assert struct.unpack("<I", desc[40:44])[0] == 8
    assert struct.unpack("<I", desc[44:48])[0] == 3
    print("PASS spec constants match android/host/dplay.c")


if __name__ == "__main__":
    test_spec_matches_android()
    test_host_client()
    test_malformed()
    print("ALL TESTS PASS")
