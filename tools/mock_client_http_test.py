#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Mock FreeKill TCP 客户端 + Admin HTTP 集成测试。"""

from __future__ import annotations

import http.client
import io
import json
import os
import select
import signal
import socket
import subprocess
import sys
import time
import uuid
from pathlib import Path

import cbor2
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import padding

ROOT = Path(__file__).resolve().parents[1]
CLIENT_NOTIFY = 0x412  # TYPE_NOTIFICATION | SRC_CLIENT | DEST_SERVER
MD5_EMPTY = "d41d8cd98f00b204e9800998ecf8427e"
TOKEN = "test-token-123"
GAME_PORT = 19640
HTTP_PORT = 9000


def free_tcp_port() -> int:
  with socket.socket(socket.AF_INET6, socket.SOCK_STREAM) as s:
    s.bind(("::1", 0))
    return s.getsockname()[1]

class PacketStream:
  def __init__(self, sock: socket.socket):
    self.sock = sock
    self.buf = b""

  def feed(self, data: bytes) -> None:
    self.buf += data

  def try_load(self):
    if not self.buf:
      return None
    bio = io.BytesIO(self.buf)
    try:
      obj = cbor2.load(bio)
    except cbor2.CBORDecodeEOF:
      return None
    except Exception:
      # 半包或损坏：等更多数据
      return None
    self.buf = self.buf[bio.tell() :]
    return obj

  def recv_one(self, timeout: float = 5.0):
    deadline = time.time() + timeout
    while True:
      obj = self.try_load()
      if obj is not None:
        return obj
      remain = deadline - time.time()
      if remain <= 0:
        raise TimeoutError(f"recv timeout, buf={self.buf[:80]!r}")
      r, _, _ = select.select([self.sock], [], [], remain)
      if not r:
        raise TimeoutError("select timeout")
      chunk = self.sock.recv(65536)
      if not chunk:
        raise ConnectionError("socket closed")
      self.feed(chunk)

  def recv_until(self, command: bytes, timeout: float = 5.0):
    deadline = time.time() + timeout
    got = []
    while time.time() < deadline:
      pkt = self.recv_one(max(0.1, deadline - time.time()))
      got.append(pkt)
      cmd = pkt[2]
      if isinstance(cmd, str):
        cmd = cmd.encode()
      if cmd == command:
        return pkt, got
    raise TimeoutError(f"want {command!r}, got {[p[2] for p in got]}")


def encode_notify(command: bytes, data: bytes | None = None) -> bytes:
  # 外层 command/data 必须是 CBOR byte string（服务端只回调 byte_string）
  return cbor2.dumps([-2, CLIENT_NOTIFY, command, data])


def http_req(method: str, path: str, token: str | None = TOKEN,
             body: str | None = None, headers: dict | None = None,
             conn: http.client.HTTPConnection | None = None):
  own = conn is None
  if own:
    conn = http.client.HTTPConnection("127.0.0.1", HTTP_PORT, timeout=5)
  hdrs = dict(headers or {})
  if token is not None:
    hdrs["Authorization"] = f"Bearer {token}"
  if body is not None and "Content-Type" not in hdrs:
    hdrs["Content-Type"] = "application/json"
  conn.request(method, path, body=body, headers=hdrs)
  resp = conn.getresponse()
  raw = resp.read()
  resp_headers = dict(resp.getheaders())
  if own:
    conn.close()
  text = raw.decode()
  return resp.status, resp_headers, \
    json.loads(text) if text.startswith("{") or text.startswith("[") else text


def http_post(path: str, payload: dict | None = None, token: str | None = TOKEN):
  body = json.dumps(payload if payload is not None else {})
  st, _, data = http_req("POST", path, token=token, body=body)
  return st, data


def http_get(path: str, token: str | None = TOKEN):
  st, _, body = http_req("GET", path, token=token)
  return st, body


class MockClient:
  def __init__(self, name: str, password: str = "pass1234", device_uuid: str | None = None):
    self.name = name
    self.password = password
    self.device_uuid = device_uuid or f"mock-{uuid.uuid4()}"
    self.sock = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
    self.sock.settimeout(10)
    self.stream: PacketStream | None = None
    self.player_id: int | None = None

  def connect(self, host: str = "::1", port: int = GAME_PORT):
    self.sock.connect((host, port, 0, 0))
    self.stream = PacketStream(self.sock)

  def login(self, md5_hex: str = MD5_EMPTY, version: bytes = b"0.5.19"):
    assert self.stream is not None
    pkt = self.stream.recv_one()
    assert pkt[2] in (b"NetworkDelayTest", "NetworkDelayTest"), pkt
    pem = cbor2.loads(pkt[3])
    if isinstance(pem, str):
      pem = pem.encode()
    pub = serialization.load_pem_public_key(pem)

    plain = os.urandom(32) + self.password.encode()
    cipher = pub.encrypt(plain, padding.PKCS1v15())
    setup = b"".join(
      cbor2.dumps(x)
      for x in [
        self.name.encode(),
        cipher,
        md5_hex.encode(),
        version,
        self.device_uuid.encode(),
      ]
    )
    self.sock.sendall(encode_notify(b"Setup", setup))

    setup_pkt, _ = self.stream.recv_until(b"Setup")
    info = cbor2.loads(setup_pkt[3])
    self.player_id = int(info[0])
    self.stream.recv_until(b"EnterLobby")
    return self.player_id

  def create_room(self, name: str, capacity: int = 8, timeout: int = 120,
                  game_mode: str = "aaa_role_mode", password: str = ""):
    # CreateRoom 内部：name / settings map 的 key·value 须为 CBOR text string
    body = cbor2.dumps([name, capacity, timeout, {"gameMode": game_mode, "password": password}])
    self.sock.sendall(encode_notify(b"CreateRoom", body))
    assert self.stream is not None
    self.stream.recv_until(b"EnterRoom", timeout=8)

  def enter_room(self, room_id: int, password: str = ""):
    body = cbor2.dumps([room_id, password])
    self.sock.sendall(encode_notify(b"EnterRoom", body))
    assert self.stream is not None
    self.stream.recv_until(b"EnterRoom", timeout=8)

  def ready(self):
    self.sock.sendall(encode_notify(b"Ready", None))

  def heartbeat(self):
    self.sock.sendall(encode_notify(b"Heartbeat", None))

  def close(self):
    try:
      self.sock.close()
    except OSError:
      pass


def start_server(game_port: int) -> subprocess.Popen:
  bin_path = ROOT / "freekill-asio"
  if not bin_path.exists():
    bin_path = ROOT / "build" / "freekill-asio"
  proc = subprocess.Popen(
    [str(bin_path), "-p", str(game_port)],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT,
    cwd=str(ROOT),
    text=True,
    bufsize=1,
  )
  # 等 HTTP 就绪
  deadline = time.time() + 8
  while time.time() < deadline:
    if proc.poll() is not None:
      out = proc.stdout.read() if proc.stdout else ""
      raise RuntimeError(f"server exited early: {out}")
    try:
      status, _ = http_get("/api/players")
      if status in (200, 401):
        return proc
    except OSError:
      time.sleep(0.1)
  raise TimeoutError("admin http not ready")


def assert_eq(a, b, msg=""):
  if a != b:
    raise AssertionError(f"{msg}: {a!r} != {b!r}")


def main() -> int:
  os.chdir(ROOT)
  game_port = free_tcp_port()
  proc = start_server(game_port)
  clients: list[MockClient] = []
  try:
    # --- 登录两个客户端（每次换名，避免脏库干扰）---
    suffix = uuid.uuid4().hex[:6]
    a = MockClient(f"alice_{suffix}")
    b = MockClient(f"bob_{suffix}")
    clients.extend([a, b])
    a.connect(port=game_port)
    b.connect(port=game_port)
    aid = a.login()
    bid = b.login()
    print(f"logged in alice={aid} bob={bid} port={game_port}")

    # 大厅应能看到两人
    time.sleep(0.2)
    st, body = http_get("/api/players")
    assert_eq(st, 200)
    names = {p["screenName"] for p in body["data"]["players"]}
    assert a.name in names and b.name in names, names
    print("PASS /api/players sees both")

    st, lobby = http_get("/api/rooms/0")
    assert_eq(st, 200)
    lobby_names = {p["screenName"] for p in lobby["data"]["players"]}
    assert a.name in lobby_names and b.name in lobby_names, lobby_names
    print("PASS /api/rooms/0 lobby")

    # --- alice 建带密码房间 ---
    a.create_room("PwRoom", password="s3cret")
    time.sleep(0.3)

    st, rooms = http_get("/api/rooms")
    assert_eq(st, 200)
    assert len(rooms["data"]) >= 1, rooms
    room = rooms["data"][0]
    room_id = room["id"]
    assert_eq(room["name"], "PwRoom")
    assert_eq(room["password"], "******", "password should be masked")
    assert room["playerCount"] >= 1
    print(f"PASS /api/rooms list room_id={room_id} masked pw")

    st, detail = http_get(f"/api/rooms/{room_id}")
    assert_eq(st, 200)
    detail_names = {p["screenName"] for p in detail["data"]["players"]}
    assert a.name in detail_names, detail
    assert_eq(detail["data"]["room"]["password"], "******")
    print("PASS /api/rooms/{id} detail")

    # bob 进房
    b.enter_room(room_id, password="s3cret")
    time.sleep(0.3)
    st, detail2 = http_get(f"/api/rooms/{room_id}")
    assert_eq(st, 200)
    names2 = {p["screenName"] for p in detail2["data"]["players"]}
    assert names2 >= {a.name, b.name}, names2
    assert_eq(detail2["data"]["room"]["playerCount"], 2)
    print("PASS both players in room via HTTP")

    # 大厅应变空（或至少少了这两人）
    st, lobby2 = http_get("/api/rooms/0")
    lobby_names2 = {p["screenName"] for p in lobby2["data"]["players"]}
    assert a.name not in lobby_names2 and b.name not in lobby_names2, lobby_names2
    print("PASS lobby empty after join")

    # ready + stat
    a.ready()
    b.ready()
    time.sleep(0.2)
    st, stat = http_get("/api/server/stat")
    assert_eq(st, 200)
    assert stat["data"]["playerCount"] == 2
    print("PASS /api/server/stat playerCount=2")

    # ================= 写操作（Phase 2 / 3）=================

    st, body = http_post("/api/broadcast", {"message": "hello-all"})
    assert_eq(st, 200, body)
    assert_eq(body["success"], True)
    print("PASS POST /api/broadcast")

    st, body = http_post(f"/api/rooms/{room_id}/broadcast", {"message": "hello-room"})
    assert_eq(st, 200, body)
    print("PASS POST /api/rooms/{id}/broadcast")

    st, body = http_post("/api/broadcast", {})
    assert_eq(st, 400)
    assert_eq(body["error"]["code"], "BAD_REQUEST")
    print("PASS POST /api/broadcast missing message -> 400")

    st, body = http_post("/api/players/kick", {"name": b.name})
    assert_eq(st, 200, body)
    time.sleep(0.4)
    st, players = http_get("/api/players")
    names_after_kick = {p["screenName"] for p in players["data"]["players"]}
    assert b.name not in names_after_kick, names_after_kick
    assert a.name in names_after_kick
    print("PASS POST /api/players/kick")

    st, body = http_post("/api/players/kick", {"name": "nobody_here"})
    assert_eq(st, 404)
    print("PASS kick missing player -> 404")

    st, body = http_post(f"/api/rooms/{room_id}/kill")
    assert_eq(st, 200, body)
    time.sleep(0.4)
    st, rooms_after = http_get("/api/rooms")
    assert all(r["id"] != room_id for r in rooms_after["data"]), rooms_after
    print("PASS POST /api/rooms/{id}/kill")

    st, body = http_post("/api/lobby/check")
    assert_eq(st, 200, body)
    print("PASS POST /api/lobby/check")

    # 账号：再登一个号做 ban / temp-ban / whitelist / reset-password
    c = MockClient(f"carol_{suffix}")
    clients.append(c)
    c.connect(port=game_port)
    c.login()
    time.sleep(0.2)

    st, body = http_post("/api/accounts/ban", {"names": [c.name]})
    assert_eq(st, 200, body)
    time.sleep(0.3)
    st, players = http_get("/api/players")
    assert c.name not in {p["screenName"] for p in players["data"]["players"]}
    print("PASS POST /api/accounts/ban (kicks online)")

    st, body = http_post("/api/accounts/unban", {"names": [c.name]})
    assert_eq(st, 200, body)
    print("PASS POST /api/accounts/unban")

    st, body = http_post("/api/accounts/temp-ban", {"name": c.name, "duration": "1m"})
    assert_eq(st, 200, body)
    print("PASS POST /api/accounts/temp-ban")

    st, body = http_post("/api/accounts/unban", {"names": [c.name]})
    assert_eq(st, 200, body)

    st, body = http_post("/api/accounts/temp-mute", {"name": c.name, "duration": "bad"})
    assert_eq(st, 400)
    print("PASS temp-mute bad duration -> 400")

    st, body = http_post("/api/accounts/whitelist", {"action": "add", "names": [c.name]})
    assert_eq(st, 200, body)
    st, body = http_post("/api/accounts/whitelist", {"action": "rm", "names": [c.name]})
    assert_eq(st, 200, body)
    print("PASS POST /api/accounts/whitelist add/rm")

    st, body = http_post("/api/accounts/reset-password", {"names": [c.name]})
    assert_eq(st, 200, body)
    print("PASS POST /api/accounts/reset-password")

    st, body = http_post("/api/server/reload-config")
    assert_eq(st, 200, body)
    print("PASS POST /api/server/reload-config")

    # 断线后玩家状态变化（至少 HTTP 仍可用）
    a.close()
    try:
      b.close()
    except Exception:
      pass
    c.close()
    clients.clear()
    time.sleep(0.5)
    st, _ = http_get("/api/players")
    assert_eq(st, 200)
    print("PASS HTTP still ok after client disconnect")

    # ================= 负面与边界场景 =================

    # --- 鉴权 ---
    st, body = http_get("/api/players", token=None)
    assert_eq(st, 401)
    assert_eq(body["success"], False)
    assert_eq(body["error"]["code"], "UNAUTHORIZED")
    st, _ = http_get("/api/players", token="wrong-token")
    assert_eq(st, 401)
    print("PASS auth: no/wrong token -> 401 UNAUTHORIZED")

    # --- 未知路由与错误信封 ---
    st, body = http_get("/api/no-such-route")
    assert_eq(st, 404)
    assert_eq(body["success"], False)
    assert_eq(body["error"]["code"], "NOT_FOUND")
    print("PASS unknown route -> 404 envelope")

    # --- 非法 / 不存在房间 ID ---
    for bad in ("abc", "-1", "12x"):
      st, body = http_get(f"/api/rooms/{bad}")
      assert_eq(st, 400, bad)
      assert_eq(body["error"]["code"], "BAD_REQUEST", bad)
    st, body = http_get("/api/rooms/99999")
    assert_eq(st, 404)
    print("PASS invalid/nonexistent room id -> 400/404")

    # --- 帮助页 ---
    st, hdrs, _ = http_req("GET", "/api")
    assert_eq(st, 200)
    assert "text/html" in hdrs.get("Content-Type", ""), hdrs
    print("PASS GET /api help page")

    # --- CORS ---
    st, hdrs, _ = http_req("GET", "/api/players",
                           headers={"Origin": "http://localhost:3000"})
    assert_eq(st, 200)
    assert_eq(hdrs.get("Access-Control-Allow-Origin"), "http://localhost:3000")
    st, hdrs, _ = http_req("GET", "/api/players",
                           headers={"Origin": "http://evil.example.com"})
    assert hdrs.get("Access-Control-Allow-Origin") is None, hdrs
    st, hdrs, _ = http_req("OPTIONS", "/api/players", token=None, headers={
        "Origin": "http://localhost:3000",
        "Access-Control-Request-Method": "GET"})
    assert_eq(st, 204)
    assert_eq(hdrs.get("Access-Control-Allow-Origin"), "http://localhost:3000")
    print("PASS CORS whitelist + preflight")

    # --- keep-alive：同一连接连续请求 ---
    conn = http.client.HTTPConnection("127.0.0.1", HTTP_PORT, timeout=5)
    st1, _, _ = http_req("GET", "/api/players", conn=conn)
    st2, _, _ = http_req("GET", "/api/rooms", conn=conn)
    conn.close()
    assert_eq((st1, st2), (200, 200))
    print("PASS keep-alive multiple requests")

    # --- query string 不影响路由 ---
    st, _ = http_get("/api/rooms?foo=bar")
    assert_eq(st, 200)
    print("PASS query string ignored")

    # --- POST 到 GET-only 路由应 405（设计文档 §5.2）---
    st, _, body = http_req("POST", "/api/players", body="{}")
    assert_eq(st, 405)
    assert_eq(body["error"]["code"], "METHOD_NOT_ALLOWED")
    st, _, _ = http_req("POST", "/api/rooms/1", body="{}")
    assert_eq(st, 405, "dynamic route method mismatch")
    print("PASS method not allowed -> 405")

    # --- body 上限 64KiB：超限应返回 413 ---
    big = "x" * (64 * 1024 + 1)
    st, _, body = http_req("POST", "/api/players", body=big)
    assert_eq(st, 413)
    assert_eq(body["error"]["code"], "PAYLOAD_TOO_LARGE")
    print("PASS oversized body -> 413")

    print("\nALL MOCK CLIENT HTTP TESTS PASSED")
    return 0
  finally:
    for c in clients:
      c.close()
    proc.send_signal(signal.SIGTERM)
    try:
      proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
      proc.kill()


if __name__ == "__main__":
  sys.exit(main())
