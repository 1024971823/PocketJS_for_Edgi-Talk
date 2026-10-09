"""Local page for the Edgi Talk board.

`edgitalk.py console` (and `gui`) serves this directory's console.html on 127.0.0.1 and opens it
in the browser. The page never talks to the board itself; every request stays on this machine
and the process forwards it with the same code as the command line.
"""

from __future__ import annotations

import ipaddress
import json
import os
import subprocess
import threading
import time
import webbrowser
from concurrent.futures import ThreadPoolExecutor, as_completed
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Callable
from urllib.parse import urlparse

import edgitalk
import stats as pcstats

HERE = os.path.dirname(os.path.abspath(__file__))
PAGE = os.path.join(HERE, "console.html")

NOT_FOUND = "没有找到板子。确认它和这台电脑在同一个 Wi-Fi。"
NEED_CODE = "先填写设置页右侧的六位配对码。"
NEED_FILE = "先选择一个 MIDI 或谱面 JSON。"
NEED_CHART = "先选择曲子，确认下面的音符数，再推到板子。"


def local_interfaces() -> list[tuple[ipaddress.IPv4Address, ipaddress.IPv4Network]]:
    """This machine's IPv4 addresses and the networks they sit on. Loopback is left out."""
    try:
        raw = subprocess.check_output(["ip", "-json", "-4", "addr"], text=True, timeout=2)
        records = json.loads(raw)
    except (OSError, subprocess.SubprocessError, ValueError):
        return []
    found = []
    for record in records:
        if record.get("ifname") == "lo":
            continue
        for item in record.get("addr_info") or []:
            if item.get("family") != "inet":
                continue
            try:
                address = ipaddress.IPv4Address(item["local"])
                network = ipaddress.ip_network(f"{item['local']}/{item['prefixlen']}", strict=False)
            except (KeyError, ValueError):
                continue
            found.append((address, network))
    return found


def blocks_near(mine: ipaddress.IPv4Address, network: ipaddress.IPv4Network, limit: int = 24) -> list[ipaddress.IPv4Network]:
    if network.prefixlen >= 24:
        return [network]
    origin = int(ipaddress.ip_network(f"{mine}/24", strict=False).network_address)
    found: list[ipaddress.IPv4Network] = []
    for dist in range(limit):
        for sign in ((0,) if dist == 0 else (1, -1)):
            raw = origin + sign * dist * 256
            if raw < 0 or raw > 0xFFFFFFFF:
                continue
            block = ipaddress.ip_network(f"{ipaddress.IPv4Address(raw)}/24", strict=False)
            if block.subnet_of(network) and block not in found:
                found.append(block)
    return found


def probe_board(ip: str, port: int, timeout: float = 0.35) -> dict[str, Any] | None:
    try:
        info = edgitalk.Board(ip, "", port=port, timeout=timeout).status()
    except edgitalk.CompanionError:
        return None
    if info.get("device") == "edgi-talk":
        info["ip"] = ip
        return info
    return None


def _first_hit(hosts: list[str], port: int) -> dict[str, Any] | None:
    pool = ThreadPoolExecutor(max_workers=64)
    futures = [pool.submit(probe_board, host, port) for host in hosts]
    try:
        for future in as_completed(futures):
            hit = future.result()
            if hit:
                return hit
    finally:
        pool.shutdown(wait=False, cancel_futures=True)
    return None


def search_lan(port: int, prefer: str = "", discover: Callable[[], list[dict[str, Any]]] | None = None) -> dict[str, Any] | None:
    """Find one board: saved address, UDP reply, then nearby addresses on this network."""
    if prefer:
        hit = probe_board(prefer, port, timeout=0.6)
        if hit:
            return hit
    cards = (discover or (lambda: edgitalk.discover(timeout=2.0)))()
    if cards:
        return cards[0]
    for mine, network in local_interfaces():
        for block in blocks_near(mine, network):
            hosts = [str(host) for host in block.hosts() if host != mine]
            hit = _first_hit(hosts, port)
            if hit:
                return hit
    return None


def chart_summary(chart: dict[str, Any]) -> str:
    size = len(edgitalk.encode_chart(chart))
    return f"{chart.get('bpm', 0)} BPM，{len(chart.get('notes', [])) // 2} 个音符，{size / 1024:.1f} KB"


def parse_multipart(body: bytes, content_type: str) -> dict[str, tuple[str, bytes]]:
    """One file plus text fields. Returns name -> (filename, bytes)."""
    marker = ""
    for piece in content_type.split(";"):
        piece = piece.strip()
        if piece.startswith("boundary="):
            marker = piece.split("=", 1)[1].strip().strip('"')
    if not marker:
        raise edgitalk.CompanionError("上传没有 boundary")
    parts: dict[str, tuple[str, bytes]] = {}
    for chunk in body.split(b"--" + marker.encode("ascii")):
        if not chunk or chunk in (b"--", b"--\r\n") or chunk.startswith(b"--"):
            continue
        chunk = chunk.removeprefix(b"\r\n").removesuffix(b"\r\n")
        head, sep, data = chunk.partition(b"\r\n\r\n")
        if not sep:
            continue
        header = head.decode("utf-8", "replace")
        name = ""
        filename = ""
        for item in header.split(";"):
            item = item.strip()
            if item.startswith("name="):
                name = item.split("=", 1)[1].strip().strip('"')
            elif item.startswith("filename="):
                filename = item.split("=", 1)[1].strip().strip('"')
        if name:
            parts[name] = (filename, data.removesuffix(b"\r\n"))
    return parts


class Console:
    """Board connection, the sender thread, and the chart waiting to be pushed."""

    def __init__(self, host: str = "", token: str = "", port: int = edgitalk.HTTP_PORT,
                 discover: Callable[[], list[dict[str, Any]]] | None = None,
                 save: Callable[[dict[str, Any]], None] | None = None,
                 config: dict[str, Any] | None = None,
                 autosearch: bool = True) -> None:
        saved = edgitalk.load_config() if config is None else config
        self.host = host or str(saved.get("host", ""))
        self.token = token or str(saved.get("token", ""))
        self.port = port
        # None means the real search (broadcast, then nearby addresses). Tests pass a stub.
        self._discover = discover
        self._save = save or edgitalk.save_config
        self.connected = False
        self.searching = False
        self.ble = ""
        self.device = ""
        self.find_note = ""
        self.send_note = ""
        self.chart_note = "选一个 MIDI 或谱面 JSON。"
        self.song_note = ""
        self.music_note = ""
        self.music_phase = ""
        self.music_sent = 0
        self.music_total = 0
        self.sending = False
        self.interval = 2
        self.stats: dict[str, Any] | None = None
        self.chart: dict[str, Any] | None = None
        self._lock = threading.Lock()
        self._xfer = threading.Lock()
        self._stop = threading.Event()
        self._wake = threading.Event()
        self._thread = threading.Thread(target=self._loop, name="edgitalk-console", daemon=True)
        self._thread.start()
        if autosearch:
            threading.Thread(target=self.locate, name="edgitalk-find", daemon=True).start()

    def close(self) -> None:
        self._stop.set()
        self._wake.set()

    def _board(self) -> edgitalk.Board:
        if not self.token.strip():
            raise edgitalk.CompanionError(NEED_CODE)
        if not self.host.strip():
            raise edgitalk.CompanionError(NOT_FOUND)
        return edgitalk.Board(self.host.strip(), self.token.strip(), port=self.port, timeout=6.0)

    def _remember(self) -> None:
        self._save({"host": self.host.strip(), "token": self.token.strip()})

    def snapshot(self) -> dict[str, Any]:
        with self._lock:
            stats = dict(self.stats) if self.stats else None
            return {
                "ok": True,
                "host": self.host,
                "token": self.token,
                "connected": self.connected,
                "device": self.device,
                "searching": self.searching,
                "link": self._link(),
                "findNote": self.find_note,
                "sending": self.sending,
                "interval": self.interval,
                "stats": stats,
                "sendNote": self.send_note,
                "chartNote": self.chart_note,
                "songNote": self.song_note,
                "musicNote": self.music_note,
                "musicProgress": {
                    "active": self.music_phase in ("pack", "send", "write"),
                    "phase": self.music_phase,
                    "sent": self.music_sent,
                    "total": self.music_total,
                },
            }

    def _link(self) -> str:
        if self.searching and self.host:
            return f"正在确认 {self.host}…"
        if self.searching:
            return "正在这个网络里找板子…"
        if self.connected and self.ble:
            return f"已连上蓝牙 {self.ble}"
        if self.connected and self.device:
            return f"已连上 {self.device} · {self.host}"
        if self.ble:
            return f"找到蓝牙 {self.ble}"
        if self.host:
            return f"找到板子 {self.host}"
        return "还没找到板子"

    def locate(self) -> dict[str, Any]:
        with self._lock:
            self.searching = True
            self.find_note = ""
            previous = self.host
        try:
            no_bluetooth = False
            if self._discover is not None:
                cards = self._discover()
                found = cards[0] if cards else None
            else:
                import ble
                no_bluetooth = not ble.adapter_present()
                mac = None if no_bluetooth else ble.find_edgitalk()
                found = {"ip": "", "name": "EdgiTalk", "ble": mac} if mac else search_lan(self.port, prefer=self.host)
        finally:
            with self._lock:
                self.searching = False
        with self._lock:
            if found and found.get("ble"):
                self.ble = str(found["ble"])
                self.device = "EdgiTalk"
                self.find_note = ""
            elif found and found.get("ip"):
                self.host = str(found["ip"])
                self.device = str(found.get("name") or self.device or "板子")
                self.find_note = ""
            else:
                self.host = ""
                self.connected = False
                if no_bluetooth and not previous:
                    self.find_note = "这台电脑没有蓝牙。"
                else:
                    self.find_note = (
                        f"{previous} 的端口 80 没有应答。这台电脑问它的 MAC 地址没人回，"
                        "Mira 多半不让两台设备互相访问。可改连板子热点 EdgiTalk，用 192.168.169.1。"
                        if previous else NOT_FOUND
                    )
        snap = self.snapshot()
        snap["ok"] = bool(self.host or self.ble) and not self.find_note
        if self.find_note:
            snap["error"] = self.find_note
        return snap

    def connect(self, token: str) -> dict[str, Any]:
        self.token = token.strip()
        if not self.token:
            with self._lock:
                self.find_note = NEED_CODE
            raise edgitalk.CompanionError(NEED_CODE)
        if not self.host and not self.ble:
            self.locate()
        if self.ble:
            import ble
            try:
                ble.ensure_link(self.ble)
            except edgitalk.CompanionError as error:
                with self._lock:
                    self.connected = False
                    self.find_note = str(error)
                raise
            self._remember()
            with self._lock:
                self.connected = True
                self.device = "EdgiTalk"
                self.find_note = ""
                self.song_note = "状态走蓝牙。游戏谱面仍用 Wi-Fi。"
                self.music_note = "主页播放器的歌走蓝牙。Wi-Fi 同时开着。"
            return self.snapshot()
        try:
            info = self._board().status()
        except edgitalk.CompanionError:
            self.locate()
            if self.ble:
                return self.connect(self.token)
            info = self._board().status()
        self._remember()
        with self._lock:
            self.connected = True
            self.device = str(info.get("name") or "板子")
            self.find_note = ""
            song = info.get("song") or {}
            self.song_note = "板上已有一首自定义曲。" if song.get("present") else "板上还没有自定义曲。"
        return self.snapshot()

    def set_interval(self, seconds: int) -> dict[str, Any]:
        self.interval = 1 if seconds <= 1 else 2 if seconds <= 2 else 5
        self._wake.set()
        return self.snapshot()

    def set_sending(self, on: bool) -> dict[str, Any]:
        if on:
            if not self.token.strip():
                raise edgitalk.CompanionError(NEED_CODE)
            if not self.ble:
                self._board()
            self._remember()
        with self._lock:
            self.sending = on
            self.send_note = "正在发给板子。" if on else "已停止。"
        self._wake.set()
        return self.snapshot()

    def sync_time(self) -> dict[str, Any]:
        payload = pcstats.collect(pcstats.Sampler())
        self._board().push_stats(payload)
        self._remember()
        with self._lock:
            self.send_note = f"已把电脑时间发给板子（{time.strftime('%H:%M:%S')}）。"
        return self.snapshot()

    def store_chart(self, filename: str, raw: bytes, title: str, level: int) -> dict[str, Any]:
        if not raw:
            raise edgitalk.CompanionError(NEED_FILE)
        level = 1 if level <= 1 else 3 if level >= 3 else 2
        title = "".join(ch if 32 <= ord(ch) < 127 else "?" for ch in title).strip()[:12]
        if filename.lower().endswith((".mid", ".midi")):
            chart = edgitalk.chart_from_midi(raw, title=title or "Custom", level=level)
        else:
            try:
                chart = json.loads(raw.decode("utf-8"))
            except (UnicodeError, ValueError) as error:
                raise edgitalk.CompanionError(f"这不是谱面 JSON：{error}") from None
            if title:
                chart["title"] = title
            problems = edgitalk.validate_chart(chart)
            if problems:
                raise edgitalk.CompanionError("；".join(problems[:3]))
        with self._lock:
            self.chart = chart
            self.chart_note = chart_summary(chart)
            self.song_note = ""
        return self.snapshot()

    def push(self) -> dict[str, Any]:
        with self._lock:
            chart = self.chart
        if chart is None:
            raise edgitalk.CompanionError(NEED_CHART)
        payload = edgitalk.encode_chart(chart)
        size = int(self._board().push(payload).get("size") or len(payload))
        self._remember()
        with self._lock:
            self.connected = True
            self.song_note = f"已推到板子：{chart.get('title', 'Custom')}，{size} 字节。"
        return self.snapshot()

    def _music_progress(self, phase: str, sent: int, total: int) -> None:
        if phase == "pack":
            text = "正在压缩…"
        elif phase == "write":
            text = "正在写入板子的 flash…"
        elif phase == "send" and total > 0:
            percent = min(100, sent * 100 // total)
            text = f"正在传输 {percent}%（{sent // 1024} / {total // 1024} KB）"
        else:
            text = ""
        with self._lock:
            self.music_phase = phase
            self.music_sent = sent
            self.music_total = total
            if text:
                self.music_note = text

    def _read_local_file(self, path: str) -> tuple[str, bytes]:
        text = os.path.expanduser(str(path or "").strip())
        if not text or not os.path.isfile(text):
            raise edgitalk.CompanionError("找不到这个文件")
        with open(text, "rb") as handle:
            return os.path.basename(text), handle.read()

    def push_music_path(self, path: str, title: str) -> dict[str, Any]:
        filename, raw = self._read_local_file(path)
        return self.push_music(filename, raw, title)

    def store_chart_path(self, path: str, title: str, level: int) -> dict[str, Any]:
        filename, raw = self._read_local_file(path)
        return self.store_chart(filename, raw, title, level)

    def push_music(self, filename: str, raw: bytes, title: str) -> dict[str, Any]:
        if not raw:
            raise edgitalk.CompanionError("先选择一首歌。")
        if not self.ble:
            self.locate()
        if not self.ble:
            raise edgitalk.CompanionError("主页播放器要先连上板子的蓝牙。")
        try:
            self._music_progress("pack", 0, 0)
            wav, name = edgitalk.prepare_player_wav(raw, filename, title)

            def tick(sent: int, total: int) -> None:
                phase = "write" if total and sent >= total else "send"
                self._music_progress(phase, sent, total)

            import ble
            with self._xfer:
                ble.push_music(self.ble, wav, self.token, name, progress=tick)
        except Exception:
            self._music_progress("", 0, 0)
            with self._lock:
                self.music_note = ""
            raise
        self._music_progress("", len(wav), len(wav))
        with self._lock:
            self.music_note = f"已传到主页播放器：{name}，{len(wav)} 字节。"
        return self.snapshot()

    def pull(self) -> bytes:
        data = self._board().pull()
        with self._lock:
            self.song_note = f"已从板子取回 {len(data)} 字节。"
        return data

    def clear(self) -> dict[str, Any]:
        self._board().clear()
        self._remember()
        with self._lock:
            self.song_note = "板上的自定义曲已删掉。"
        return self.snapshot()

    def scores(self) -> dict[str, Any]:
        data = self._board().scores()
        names = ["Sunny Steps", "Neon Pulse", "Cat Rush", "Custom"]
        bits = []
        for slot, best in enumerate(data.get("best", [])[:4]):
            rank = (data.get("rank") or "")[slot:slot + 1] or "-"
            bits.append(f"{names[slot]} {best} {rank}")
        with self._lock:
            self.song_note = "最高分：" + "，".join(bits)
        return self.snapshot()

    def _loop(self) -> None:
        sampler = pcstats.Sampler()
        time.sleep(0.4)
        elapsed = self.interval
        while not self._stop.is_set():
            payload = pcstats.collect(sampler)
            with self._lock:
                self.stats = payload
                sending = self.sending
                every = self.interval
                board = edgitalk.Board(self.host.strip(), self.token.strip(), port=self.port, timeout=6.0) if sending else None
            if sending and elapsed >= every and (self.ble or board is not None):
                elapsed = 0
                try:
                    if self.ble:
                        import ble
                        sent = dict(payload)
                        sent["token"] = self.token
                        with self._xfer:
                            ble.write_stats(self.ble, sent)
                    else:
                        board.push_stats(payload)
                    note = f"已发送 {time.strftime('%H:%M:%S')}"
                except edgitalk.CompanionError as error:
                    note = str(error)
                with self._lock:
                    self.send_note = note
            elapsed += 1
            self._wake.wait(1.0)
            self._wake.clear()


def _json(handler: BaseHTTPRequestHandler, code: int, body: dict[str, Any]) -> None:
    data = json.dumps(body, ensure_ascii=False).encode("utf-8")
    handler.send_response(code)
    handler.send_header("Content-Type", "application/json; charset=utf-8")
    handler.send_header("Content-Length", str(len(data)))
    handler.send_header("Cache-Control", "no-store")
    handler.end_headers()
    handler.wfile.write(data)


def make_handler(app: Console) -> type[BaseHTTPRequestHandler]:
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, format: str, *args: Any) -> None:
            return

        def _read(self) -> bytes:
            length = int(self.headers.get("Content-Length") or 0)
            return self.rfile.read(length) if length else b""

        def _guard(self, action: Callable[[], Any]) -> None:
            try:
                result = action()
            except edgitalk.CompanionError as error:
                _json(self, 400, {"ok": False, "error": str(error)})
                return
            except (OSError, ValueError) as error:
                _json(self, 400, {"ok": False, "error": str(error)})
                return
            if isinstance(result, bytes):
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Disposition", 'attachment; filename="board-song.json"')
                self.send_header("Content-Length", str(len(result)))
                self.end_headers()
                self.wfile.write(result)
                return
            _json(self, 200, result)

        def do_GET(self) -> None:
            path = urlparse(self.path).path
            if path in ("/", "/console.html"):
                page = open(PAGE, "rb").read()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(page)))
                self.end_headers()
                self.wfile.write(page)
                return
            if path == "/api/local/state":
                _json(self, 200, app.snapshot())
                return
            if path == "/api/local/pull":
                self._guard(app.pull)
                return
            _json(self, 404, {"ok": False, "error": "没有这个页面"})

        def do_POST(self) -> None:
            path = urlparse(self.path).path
            raw = self._read()
            kind = self.headers.get("Content-Type", "")
            if path == "/api/local/music":
                def store_music() -> dict[str, Any]:
                    fields = parse_multipart(raw, kind)
                    filename, data = fields.get("file", ("", b""))
                    title = fields.get("title", ("", b""))[1].decode("utf-8", "replace")
                    return app.push_music(filename, data, title)
                self._guard(store_music)
                return
            if path == "/api/local/chart":
                def store() -> dict[str, Any]:
                    fields = parse_multipart(raw, kind)
                    filename, data = fields.get("file", ("", b""))
                    title = fields.get("title", ("", b""))[1].decode("utf-8", "replace")
                    try:
                        level = int(fields.get("level", ("", b"2"))[1] or b"2")
                    except ValueError:
                        level = 2
                    return app.store_chart(filename, data, title, level)
                self._guard(store)
                return
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
            except ValueError:
                _json(self, 400, {"ok": False, "error": "请求不是 JSON"})
                return
            if not isinstance(payload, dict):
                payload = {}
            routes: dict[str, Callable[[], dict[str, Any]]] = {
                "/api/local/connect": lambda: app.connect(str(payload.get("token", ""))),
                "/api/local/music-path": lambda: app.push_music_path(
                    str(payload.get("path", "")), str(payload.get("title", ""))),
                "/api/local/chart-path": lambda: app.store_chart_path(
                    str(payload.get("path", "")), str(payload.get("title", "")),
                    int(payload.get("level", 2) or 2)),
                "/api/local/discover": app.locate,
                "/api/local/interval": lambda: app.set_interval(int(payload.get("interval", 2))),
                "/api/local/send": lambda: app.set_sending(bool(payload.get("on"))),
                "/api/local/sync": app.sync_time,
                "/api/local/push": app.push,
                "/api/local/clear": app.clear,
                "/api/local/scores": app.scores,
            }
            action = routes.get(path)
            if action is None:
                _json(self, 404, {"ok": False, "error": "没有这个操作"})
                return
            self._guard(action)

    return Handler


def serve(app: Console, open_browser: bool = True, listen_port: int = 0) -> ThreadingHTTPServer:
    server = ThreadingHTTPServer(("127.0.0.1", listen_port), make_handler(app))
    if open_browser:
        webbrowser.open(f"http://127.0.0.1:{server.server_port}/")
    print(f"Edgi Talk  http://127.0.0.1:{server.server_port}/   Ctrl-C 退出")
    return server


def run_console(host: str = "", token: str = "", port: int = edgitalk.HTTP_PORT, open_browser: bool = True,
                listen_port: int = 0) -> int:
    app = Console(host=host, token=token, port=port)
    server = serve(app, open_browser=open_browser, listen_port=listen_port)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        return 0
    finally:
        app.close()
        server.shutdown()
    return 0
