"""SHL7 direct-bridge endpoint: Python prototype of the Windows provider.

Implements the exact wire protocol used by the Android port
(`pc_decomp_strhld/android/host/dplay.c`) so a Windows DirectPlay
Service Provider (see ../windows/Shl7PlayProvider.cpp) can talk to it
directly, without Steam. Little-endian payloads, big-endian 20-byte
packet header -- identical to dplay.c (both Android ARM64 and Windows
x86/x64 are little-endian, so native struct layouts are compatible).

Guest-facing semantics mirror DirectPlay: the endpoint queues
"guest messages" with kinds matching DPSYS_* (dplay.h):
  0     data
  3     DPSYS_CREATEPLAYERORGROUP
  5     DPSYS_DESTROYPLAYERORGROUP
  0x31  DPSYS_SESSIONLOST
  0x102 DPSYS_SETPLAYERORGROUPDATA
  0x104 DPSYS_SETSESSIONDESC
"""
import socket
import struct
import threading
import time

MAGIC = 0x53484C37
PORTS = {"classic": 47624, "extreme": 47625, "original": 47628}
MAX_DATA = 1024 * 1024
PLAYERS = 8
NAME_UNITS = 128            # uint16 units in lan_session / lan_player
PLAYER_DATA_SIZE = 4096
SESSION_SIZE = 80 + NAME_UNITS * 2          # 336
PLAYER_SIZE = 12 + NAME_UNITS * 2 + PLAYER_DATA_SIZE  # 4364
# packet kinds (dplay.c enum NET_*)
HELLO, WELCOME, PLAYER, REMOVE, DATA, SESSION, PLAYER_DATA = 1, 2, 3, 4, 5, 6, 7
# guest message kinds
G_DATA, G_CREATE, G_DESTROY, G_LOST, G_PDATA, G_SESSION = 0, 3, 5, 0x31, 0x102, 0x104


def pack_header(kind, frm, to, payload: bytes) -> bytes:
    return struct.pack(">5I", MAGIC, kind, frm, to, len(payload)) + payload


def parse_header(buf: bytes):
    """Return (kind, frm, to, payload) or None if incomplete/invalid header."""
    if len(buf) < 20:
        return None
    magic, kind, frm, to, size = struct.unpack(">5I", buf[:20])
    if magic != MAGIC or size > MAX_DATA:
        raise ValueError("bad SHL7 magic/size")
    return kind, frm, to, size


def make_session_desc(size=80, flags=0, guid_instance=b"\0" * 16,
                      guid_application=b"\0" * 16, max_players=PLAYERS,
                      current_players=0):
    words = [0] * 20
    words[0] = size
    words[1] = flags
    desc = struct.pack("<20I", *words)
    desc = desc[:8] + guid_instance + desc[24:]
    desc = desc[:24] + guid_application + desc[40:]
    desc = desc[:40] + struct.pack("<I", max_players) + desc[44:48] + desc[48:]
    # current_players is written by _refresh_session_count() on host/client;
    # the builder keeps the caller-supplied value at desc[44] only if set here.
    desc = desc[:44] + struct.pack("<I", current_players) + desc[48:]
    return desc


def make_session(desc80: bytes, name: str) -> bytes:
    name16 = name.encode("utf-16-le")[: (NAME_UNITS - 1) * 2]
    name16 = name16 + b"\0" * (NAME_UNITS * 2 - len(name16))
    return desc80 + name16


def parse_session(blob: bytes):
    desc = blob[:80]
    name = blob[80:80 + NAME_UNITS * 2].decode("utf-16-le", errors="replace").rstrip("\0")
    return desc, name


def make_player(pid, flags, name, data=b""):
    name16 = name.encode("utf-16-le")[: (NAME_UNITS - 1) * 2]
    name16 = name16 + b"\0" * (NAME_UNITS * 2 - len(name16))
    data = data[:PLAYER_DATA_SIZE]
    return struct.pack("<3I", pid, flags, len(data)) + name16 + data + b"\0" * (PLAYER_DATA_SIZE - len(data))


def parse_player(blob: bytes):
    pid, flags, length = struct.unpack("<3I", blob[:12])
    name = blob[12:12 + NAME_UNITS * 2].decode("utf-16-le", errors="replace").rstrip("\0")
    return pid, flags, name, blob[12 + NAME_UNITS * 2:12 + NAME_UNITS * 2 + length]


def valid_session(blob: bytes) -> bool:
    if len(blob) != SESSION_SIZE:
        return False
    desc = blob[:80]
    size, flags = struct.unpack("<2I", desc[:8])
    maxp, curp = struct.unpack("<2I", desc[40:48])
    return size == 80 and 0 < maxp <= PLAYERS and curp <= PLAYERS and blob[80 + NAME_UNITS * 2 - 2:] == b"\0\0"


def valid_player(blob: bytes) -> bool:
    if len(blob) != PLAYER_SIZE:
        return False
    pid, _flags, length = struct.unpack("<3I", blob[:12])
    return pid > 0 and length <= PLAYER_DATA_SIZE and blob[12 + NAME_UNITS * 2 - 2:12 + NAME_UNITS * 2] == b"\0\0"


def discover(app_guid: bytes, port: int, timeout=0.35, target=None):
    """UDP broadcast discovery; returns [(ip, session_blob), ...]."""
    found = []
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.settimeout(timeout)
    # dplay.c sends uint32 query[9] (36 bytes): magic + app GUID + padding.
    query = struct.pack(">I", MAGIC) + app_guid + b"\0" * 16
    addrs = ["255.255.255.255"]
    if target:
        addrs.append(target)
    for a in addrs:
        try:
            s.sendto(query, (a, port + 2))
        except OSError:
            pass
    end = time.time() + timeout
    while time.time() < end:
        try:
            data, (ip, _p) = s.recvfrom(4 + SESSION_SIZE)
        except socket.timeout:
            break
        if len(data) != 4 + SESSION_SIZE:
            continue
        if struct.unpack(">I", data[:4])[0] != MAGIC:
            continue
        blob = data[4:]
        if not valid_session(blob):
            continue
        desc, _name = parse_session(blob)
        if desc[24:40] != app_guid:  # guidApplication at desc+24
            continue
        if any(f[1][:40] == desc[:40] for f in found):
            continue
        found.append((ip, blob))
    s.close()
    return found


class Endpoint:
    """Host or client speaking SHL7, queueing guest messages."""

    def __init__(self, port):
        self.port = port
        self.hosting = False
        self.local_id = 0
        self.session = None            # session blob (336 bytes)
        self.players = {}              # id -> (flags, name, data)
        self.messages = []             # (kind, from, to, payload)
        self.lock = threading.Lock()
        self.stop = False
        self.thread = None
        self.listener = None
        self.discovery_sock = None
        self.peers = {}                # id -> {sock, welcomed, id}
        self.peer_lock = threading.Lock()
        self.next_id = 2
        self.lost = False

    # -- guest queue -------------------------------------------------
    def _queue(self, kind, frm, to, payload):
        with self.lock:
            self.messages.append((kind, frm, to, payload))

    def drain(self):
        with self.lock:
            out, self.messages = self.messages, []
            return out

    # -- packet IO ---------------------------------------------------
    def _send_packet(self, sock, kind, frm, to, payload):
        sock.sendall(pack_header(kind, frm, to, payload))

    def _recv_packets(self, sock, buf):
        """Feed bytes, return list of (kind, frm, to, payload)."""
        out = []
        while True:
            parsed = parse_header(buf)
            if parsed is None:
                return out, buf
            kind, frm, to, size = parsed
            if len(buf) < 20 + size:
                return out, buf
            payload = buf[20:20 + size]
            out.append((kind, frm, to, payload))
            buf = buf[20 + size:]

    # -- host --------------------------------------------------------
    def host(self, session_blob):
        self.session = session_blob
        self.hosting = True
        self.local_id = 1
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("0.0.0.0", self.port))
        self.listener.listen(8)
        self.listener.settimeout(0.2)
        self.discovery_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.discovery_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.discovery_sock.bind(("0.0.0.0", self.port + 2))
        self.discovery_sock.settimeout(0.2)
        self.thread = threading.Thread(target=self._host_loop, daemon=True)
        self.thread.start()

    def _host_loop(self):
        while not self.stop:
            try:
                conn, _ = self.listener.accept()
                conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                pid = self.next_id
                self.next_id += 1
                with self.peer_lock:
                    self.peers[pid] = {"sock": conn, "welcomed": False, "id": pid}
                threading.Thread(target=self._peer_loop, args=(pid, conn), daemon=True).start()
            except socket.timeout:
                pass
            except OSError:
                if self.stop:
                    break
            try:
                while True:
                    data, addr = self.discovery_sock.recvfrom(36)
                    if len(data) not in (20, 36) or struct.unpack(">I", data[:4])[0] != MAGIC:
                        continue
                    desc, _ = parse_session(self.session)
                    flags = struct.unpack("<I", desc[4:8])[0]
                    if data[4:20] == desc[24:40] and not (flags & 0x21):
                        self.discovery_sock.sendto(
                            struct.pack(">I", MAGIC) + self.session, addr)
            except socket.timeout:
                pass
            except OSError:
                if self.stop:
                    break

    def _relay(self, except_id, kind, frm, to, payload):
        with self.peer_lock:
            targets = [p for pid, p in self.peers.items()
                       if pid != except_id and p["welcomed"] and (not to or pid == to)]
        for p in targets:
            try:
                self._send_packet(p["sock"], kind, frm, to, payload)
            except OSError:
                pass

    def _peer_loop(self, pid, sock):
        buf = b""
        try:
            while not self.stop:
                chunk = sock.recv(16384)
                if not chunk:
                    break
                buf += chunk
                packets, buf = self._recv_packets(sock, buf)
                for kind, frm, to, payload in packets:
                    if not self._process_host_packet(pid, kind, frm, to, payload):
                        raise ValueError("invalid packet")
        except (OSError, ValueError):
            pass
        finally:
            self._drop_peer(pid)

    def _process_host_packet(self, pid, kind, frm, to, payload):
        with self.peer_lock:
            peer = self.peers.get(pid)
        if peer is None:
            return False
        if not peer["welcomed"]:
            if kind != HELLO or len(payload) != 32:
                return False
            desc, _ = parse_session(self.session)
            if desc[8:40] != payload:  # desc+2 .. desc+34 (guidInstance+guidApplication)
                return False
            if struct.unpack("<I", desc[4:8])[0] & 0x21:
                return False
            if len(self.players) >= struct.unpack("<I", desc[40:44])[0]:
                return False
            # dplay.c sends the whole players[8] array including empty slots.
            def _slot(i):
                if i in self.players:
                    f, n, d = self.players[i]
                    return make_player(i, f, n, d)
                return b"\0" * PLAYER_SIZE
            players_blob = b"".join(_slot(i) for i in range(PLAYERS))
            welcome = struct.pack("<I", pid) + self.session + players_blob
            peer["welcomed"] = True
            self._send_packet(peer["sock"], WELCOME, 0, pid, welcome)
            return True
        if kind == DATA:
            if frm not in self.players:
                return False
            if not to or to == self.local_id:
                self._queue(G_DATA, frm, self.local_id, payload)
            self._relay(pid, kind, frm, to, payload)
            return True
        if kind in (PLAYER, PLAYER_DATA):
            if not valid_player(payload):
                return False
            npid, _f, _n, _d = parse_player(payload)
            if npid != pid:
                return False
            created = npid not in self.players
            self.players[npid] = parse_player(payload)[1:]
            self._refresh_session()
            if npid != self.local_id:
                self._queue(G_CREATE if created else G_PDATA, 0, self.local_id, payload)
            self._relay(pid, kind, frm, 0, payload)
            return True
        return False

    def _refresh_session(self):
        desc = bytearray(self.session[:80])
        struct.pack_into("<I", desc, 44, len(self.players))
        self.session = bytes(desc) + self.session[80:]

    def _drop_peer(self, pid):
        with self.peer_lock:
            peer = self.peers.pop(pid, None)
        if peer:
            try:
                peer["sock"].close()
            except OSError:
                pass
            if pid in self.players:
                del self.players[pid]
                self._refresh_session()
                self._queue(G_DESTROY, 0, self.local_id, struct.pack("<I", pid))
                self._relay(pid, REMOVE, 0, 0, struct.pack("<I", pid))

    # -- client ------------------------------------------------------
    def join(self, ip, session_blob, timeout=5.0):
        self.session = session_blob
        sock = socket.create_connection((ip, self.port), timeout=timeout)
        sock.settimeout(None)  # create_connection leaves the timeout on; clear it
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        with self.peer_lock:
            self.peers[1] = {"sock": sock, "welcomed": False, "id": 1}
        desc, _ = parse_session(session_blob)
        self._send_packet(sock, HELLO, 0, 0, desc[8:40])
        self.thread = threading.Thread(target=self._client_loop, args=(sock,), daemon=True)
        self.thread.start()
        end = time.time() + timeout
        while time.time() < end:
            with self.peer_lock:
                if self.peers[1]["welcomed"]:
                    return True
            if self.lost:
                return False
            time.sleep(0.02)
        return False

    def _client_loop(self, sock):
        buf = b""
        try:
            while not self.stop:
                chunk = sock.recv(16384)
                if not chunk:
                    break
                buf += chunk
                packets, buf = self._recv_packets(sock, buf)
                for kind, frm, to, payload in packets:
                    self._process_client_packet(kind, frm, to, payload)
        except (OSError, ValueError):
            pass
        finally:
            if not self.stop:
                self.lost = True
                self._queue(G_LOST, 0, self.local_id, b"")

    def _process_client_packet(self, kind, frm, to, payload):
        with self.peer_lock:
            peer = self.peers.get(1)
        if peer is not None and not peer["welcomed"]:
            if kind != WELCOME or len(payload) != 4 + SESSION_SIZE + PLAYERS * PLAYER_SIZE:
                return False
            (pid,) = struct.unpack("<I", payload[:4])
            if pid < 2:
                return False
            blob = payload[4:4 + SESSION_SIZE]
            if not valid_session(blob):
                return False
            desc, _ = parse_session(blob)
            mydesc, _ = parse_session(self.session)
            if desc[8:40] != mydesc[8:40]:
                return False
            players_blob = payload[4 + SESSION_SIZE:]
            for i in range(PLAYERS):
                chunk = players_blob[i * PLAYER_SIZE:(i + 1) * PLAYER_SIZE]
                if chunk.strip(b"\0"):
                    if not valid_player(chunk):
                        return False
                    npid, fl, nm, dt = parse_player(chunk)
                    self.players[npid] = (fl, nm, dt)
            self.session = blob
            self.local_id = pid
            peer["welcomed"] = True
            return True
        if kind == DATA:
            if frm not in self.players and frm != self.local_id:
                return False
            if not to or to == self.local_id:
                self._queue(G_DATA, frm, self.local_id, payload)
            return True
        if kind in (PLAYER, PLAYER_DATA):
            if not valid_player(payload):
                return False
            npid, fl, nm, dt = parse_player(payload)
            created = npid not in self.players
            self.players[npid] = (fl, nm, dt)
            self._refresh_session_count()
            if npid != self.local_id:
                self._queue(G_CREATE if created else G_PDATA, 0, self.local_id, payload)
            return True
        if kind == REMOVE and len(payload) == 4:
            (pid,) = struct.unpack("<I", payload)
            self.players.pop(pid, None)
            self._refresh_session_count()
            self._queue(G_DESTROY, 0, self.local_id, payload)
            return True
        if kind == SESSION and len(payload) == SESSION_SIZE:
            if not valid_session(payload):
                return False
            desc, _ = parse_session(payload)
            mydesc, _ = parse_session(self.session)
            if desc[8:40] != mydesc[8:40]:
                return False
            self.session = payload
            self._queue(G_SESSION, 0, self.local_id, payload)
            return True
        return False

    def _refresh_session_count(self):
        desc = bytearray(self.session[:80])
        struct.pack_into("<I", desc, 44, len(self.players))
        self.session = bytes(desc) + self.session[80:]

    # -- guest API ---------------------------------------------------
    def create_player(self, name, flags=0, data=b""):
        blob = make_player(self.local_id, flags, name, data)
        self.players[self.local_id] = (flags, name, data)
        self._refresh_session_count()
        if self.hosting:
            self._relay(-1, PLAYER, self.local_id, 0, blob)
        else:
            with self.peer_lock:
                self._send_packet(self.peers[1]["sock"], PLAYER, self.local_id, 0, blob)
        for pid, (f, n, d) in list(self.players.items()):
            if pid != self.local_id:
                self._queue(G_CREATE, 0, self.local_id, make_player(pid, f, n, d))
        return self.local_id

    def set_player_data(self, data):
        blob = make_player(self.local_id, 0, self.players[self.local_id][1], data)
        self.players[self.local_id] = (0, self.players[self.local_id][1], data)
        if self.hosting:
            self._relay(-1, PLAYER_DATA, self.local_id, 0, blob)
        else:
            with self.peer_lock:
                self._send_packet(self.peers[1]["sock"], PLAYER_DATA, self.local_id, 0, blob)

    def send_data(self, to, payload):
        if len(payload) > MAX_DATA:
            raise ValueError("too large")
        if to == self.local_id:
            self._queue(G_DATA, self.local_id, to, payload)
        elif self.hosting:
            self._relay(-1, DATA, self.local_id, to, payload)
        else:
            with self.peer_lock:
                self._send_packet(self.peers[1]["sock"], DATA, self.local_id, to, payload)

    def set_session_desc(self, desc80):
        desc = bytearray(desc80)
        mydesc, _ = parse_session(self.session)
        desc[8:40] = mydesc[8:40]  # host keeps its session GUIDs (dplay.c read_session path)
        name = parse_session(self.session)[1]
        self.session = make_session(bytes(desc), name)
        self._refresh_session_count()
        if self.hosting:
            self._relay(-1, SESSION, 0, 0, self.session)

    def close(self):
        self.stop = True
        if self.listener:
            self.listener.close()
        if self.discovery_sock:
            self.discovery_sock.close()
        with self.peer_lock:
            for p in self.peers.values():
                try:
                    p["sock"].shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                try:
                    p["sock"].close()
                except OSError:
                    pass
