#!/usr/bin/env python3
"""Edgi Talk companion for Beat Dash.

A small, dependency-free command line (plus optional Tk window) that talks to the
board over Wi-Fi:

    edgitalk.py discover              find boards on the LAN (UDP broadcast)
    edgitalk.py pair 192.168.1.23 123456   remember host + pairing code shown in Settings
    edgitalk.py status                device / custom-song / playing state
    edgitalk.py scores                best score and rank per song slot
    edgitalk.py push chart.json       upload a custom song (Songs -> "PC" card)
    edgitalk.py pull [out.json]       download the stored custom song
    edgitalk.py clear                 delete the custom song
    edgitalk.py make-chart in.mid -o chart.json    build a chart from a MIDI file
    edgitalk.py demo -o chart.json    write a tiny example chart
    edgitalk.py validate chart.json   check a chart without a board
    edgitalk.py stats [--watch]       send this PC's CPU/RAM/disk/temperature (and clock) to the board
    edgitalk.py console               browser page: type the pair code, the board address is found for you (alias: gui)
    edgitalk.py mock                  fake board for testing (no hardware)

Only the Python standard library is used.
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from typing import Any

DISCOVERY_PORT = 47808
HTTP_PORT = 80
SONG_LIMIT = 96 * 1024
# /flash is 1 MB with 64 KB blocks. Leave room for the filesystem itself.
PLAYER_WAV_LIMIT = 380 * 1024
MAX_EVENTS = 2000
MAX_NOTES = 2000
MAX_CHART_STEPS = 65535
BPM_MIN = 60
BPM_MAX = 240
TITLE_MAX_CODE_POINTS = 64
LEVEL_MIN = 1
LEVEL_MAX = 3
PLAYER_NAME_MAX = 16
CONFIG_PATH = os.path.join(os.path.expanduser("~"), ".edgitalk.json")

VOICE_LEAD, VOICE_BASS, VOICE_DRUM = 0, 1, 2
NOTE_GROUND, NOTE_AIR, NOTE_BIG_GROUND, NOTE_BIG_AIR = 0, 1, 2, 3


class CompanionError(Exception):
    pass


# -- configuration ----------------------------------------------------------------


def load_config() -> dict[str, Any]:
    try:
        with open(CONFIG_PATH, "r", encoding="utf-8") as handle:
            return json.load(handle)
    except (OSError, ValueError):
        return {}


def save_config(config: dict[str, Any]) -> None:
    with open(CONFIG_PATH, "w", encoding="utf-8") as handle:
        json.dump(config, handle, indent=2)
    try:
        os.chmod(CONFIG_PATH, 0o600)
    except OSError:
        pass


# -- device access ----------------------------------------------------------------


def _lan_destinations() -> list[str]:
    """Places a discovery packet can actually arrive.

    255.255.255.255 is dropped by many access points (this network included). The subnet
    broadcast and a previously saved address are unicast-or-directed and get through.
    """
    found: list[str] = []

    def add(ip: str) -> None:
        if ip and ip not in found and ip != "0.0.0.0":
            found.append(ip)

    add("255.255.255.255")
    add("192.168.169.1")
    saved = load_config().get("host")
    if isinstance(saved, str):
        add(saved)
    if sys.platform.startswith("linux"):
        import fcntl

        for name in os.listdir("/sys/class/net"):
            if name == "lo":
                continue
            probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            try:
                packed = fcntl.ioctl(probe.fileno(), 0x8919, struct.pack("256s", name.encode()[:15]))
                add(socket.inet_ntoa(packed[20:24]))
            except OSError:
                pass
            finally:
                probe.close()
    return found


def discover(timeout: float = 1.5, port: int = DISCOVERY_PORT, targets: list[str] | None = None) -> list[dict[str, Any]]:
    """Broadcast an "EDGI?" datagram and collect every board that answers."""
    found: dict[str, dict[str, Any]] = {}
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.settimeout(0.25)
    destinations = targets if targets is not None else _lan_destinations()
    deadline = time.monotonic() + timeout
    next_send = 0.0
    try:
        while time.monotonic() < deadline:
            if time.monotonic() >= next_send:
                for destination in destinations:
                    try:
                        sock.sendto(b"EDGI?", (destination, port))
                    except OSError:
                        pass
                next_send = time.monotonic() + 0.5
            try:
                data, address = sock.recvfrom(512)
            except socket.timeout:
                continue
            except OSError:
                break
            try:
                card = json.loads(data.decode("utf-8", "replace"))
            except ValueError:
                continue
            if card.get("device") == "edgi-talk":
                card["ip"] = address[0]
                found[address[0]] = card
    finally:
        sock.close()
    return list(found.values())


# The board lives on the LAN: never send these requests through an HTTP proxy configured in the
# environment (http_proxy and friends), which would answer 502 for a private address.
_DIRECT = urllib.request.build_opener(urllib.request.ProxyHandler({}))


class Board:
    def __init__(self, host: str, token: str = "", port: int = HTTP_PORT, timeout: float = 10.0):
        self.base = f"http://{host}:{port}" if port != 80 else f"http://{host}"
        self.token = token
        self.timeout = timeout

    def _request(self, method: str, path: str, body: bytes | None = None, auth: bool = False) -> bytes:
        request = urllib.request.Request(self.base + path, data=body, method=method)
        if body is not None:
            request.add_header("Content-Type", "application/json")
        if auth:
            if not self.token:
                raise CompanionError("this action needs the pairing code: run `edgitalk.py pair <ip> <code>`")
            request.add_header("X-Token", self.token)
        try:
            with _DIRECT.open(request, timeout=self.timeout) as response:
                return response.read()
        except urllib.error.HTTPError as error:
            detail = error.read().decode("utf-8", "replace")
            error.close()
            try:
                detail = json.loads(detail).get("error", detail)
            except ValueError:
                pass
            raise CompanionError(f"board answered {error.code}: {detail}") from None
        except (urllib.error.URLError, socket.timeout, ConnectionError) as error:
            raise CompanionError(f"cannot reach {self.base}: {getattr(error, 'reason', error)}") from None

    def status(self) -> dict[str, Any]:
        return json.loads(self._request("GET", "/api/status"))

    def scores(self) -> dict[str, Any]:
        return json.loads(self._request("GET", "/api/scores", auth=True))

    def push(self, payload: bytes) -> dict[str, Any]:
        return json.loads(self._request("PUT", "/api/song", payload, auth=True))

    def pull(self) -> bytes:
        return self._request("GET", "/api/song", auth=True)

    def push_stats(self, payload: dict[str, Any]) -> dict[str, Any]:
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        return json.loads(self._request("PUT", "/api/pc", body, auth=True))

    def pc(self) -> dict[str, Any]:
        return json.loads(self._request("GET", "/api/pc", auth=True))

    def clear(self) -> dict[str, Any]:
        return json.loads(self._request("DELETE", "/api/song", auth=True))


# -- chart format -----------------------------------------------------------------


def _is_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def validate_chart(chart: Any) -> list[str]:
    """Validate the same custom-song contract enforced by the firmware."""
    problems: list[str] = []
    if not isinstance(chart, dict):
        return ["chart must be a JSON object"]

    bpm = chart.get("bpm")
    if not _is_int(bpm) or not BPM_MIN <= bpm <= BPM_MAX:
        problems.append(f"bpm must be an integer between {BPM_MIN} and {BPM_MAX}")

    if "title" in chart and (not isinstance(chart["title"], str) or len(chart["title"]) > TITLE_MAX_CODE_POINTS):
        problems.append(f"title must be a string of at most {TITLE_MAX_CODE_POINTS} characters")
    if "level" in chart and (not _is_int(chart["level"]) or not LEVEL_MIN <= chart["level"] <= LEVEL_MAX):
        problems.append(f"level must be an integer {LEVEL_MIN}..{LEVEL_MAX}")
    if "steps" in chart and (not _is_int(chart["steps"]) or not 1 <= chart["steps"] <= MAX_CHART_STEPS):
        problems.append(f"steps must be an integer 1..{MAX_CHART_STEPS}")

    notes = chart.get("notes")
    if (not isinstance(notes, list) or len(notes) < 2 or len(notes) % 2 or
            len(notes) // 2 > MAX_NOTES):
        problems.append(
            f"notes must contain 1..{MAX_NOTES} [step, type] pairs"
        )
    else:
        previous_step = -1
        for index in range(0, len(notes), 2):
            step, kind = notes[index:index + 2]
            if not _is_int(step) or not 0 <= step <= MAX_CHART_STEPS:
                problems.append(f"notes[{index}]: step must be an integer 0..{MAX_CHART_STEPS}")
                break
            if step < previous_step:
                problems.append(f"notes[{index}]: steps must be nondecreasing")
                break
            previous_step = step
            if not _is_int(kind) or kind not in (0, 1, 2, 3):
                problems.append(f"notes[{index + 1}]: type must be 0..3")
                break

    events = chart.get("events")
    if (not isinstance(events, list) or not events or len(events) % 5 or
            len(events) // 5 > MAX_EVENTS):
        problems.append(
            f"events must contain 1..{MAX_EVENTS} [step, voice, note, length, volume] records"
        )
    else:
        for index in range(0, len(events), 5):
            step, voice, note, length, volume = events[index:index + 5]
            if not all(_is_int(value) for value in (step, voice, note, length, volume)):
                problems.append(f"events[{index}]: all fields must be integers")
                break
            if not 0 <= step <= MAX_CHART_STEPS:
                problems.append(f"events[{index}]: step must be 0..{MAX_CHART_STEPS}")
                break
            if voice not in (0, 1, 2):
                problems.append(f"events[{index + 1}]: voice must be 0..2")
                break
            note_valid = 0 <= note <= 2 if voice == 2 else 12 <= note <= 108
            if not note_valid:
                problems.append(f"events[{index + 2}]: note out of range")
                break
            if not 1 <= length <= 255:
                problems.append(f"events[{index + 3}]: length must be 1..255")
                break
            if not 0 <= volume <= 127:
                problems.append(f"events[{index + 4}]: volume must be 0..127")
                break
    return problems


def encode_chart(chart: dict[str, Any]) -> bytes:
    payload = json.dumps(chart, separators=(",", ":")).encode("utf-8")
    if len(payload) > SONG_LIMIT:
        raise CompanionError(f"chart is {len(payload)} bytes; the board accepts at most {SONG_LIMIT}")
    return payload


# -- MIDI -> chart ------------------------------------------------------------------


def _varlen(data: bytes, pos: int) -> tuple[int, int]:
    value = 0
    while True:
        byte = data[pos]
        pos += 1
        value = (value << 7) | (byte & 0x7F)
        if not byte & 0x80:
            return value, pos


def parse_midi(data: bytes) -> dict[str, Any]:
    """Minimal Standard MIDI File reader: notes per channel plus the first tempo."""
    if data[:4] != b"MThd":
        raise CompanionError("not a MIDI file")
    _fmt, tracks, division = struct.unpack(">HHH", data[8:14])
    if division & 0x8000:
        raise CompanionError("SMPTE time division is not supported")
    pos = 14
    tempo = 500000
    tempo_seen = False
    channels: dict[int, list[tuple[int, int, int]]] = {}
    for _ in range(tracks):
        if data[pos:pos + 4] != b"MTrk":
            raise CompanionError("corrupt MIDI track")
        size = struct.unpack(">I", data[pos + 4:pos + 8])[0]
        pos += 8
        end = pos + size
        tick = 0
        status = 0
        open_notes: dict[tuple[int, int], int] = {}
        while pos < end:
            delta, pos = _varlen(data, pos)
            tick += delta
            byte = data[pos]
            if byte == 0xFF:
                kind = data[pos + 1]
                length, body = _varlen(data, pos + 2)
                if kind == 0x51 and length == 3 and not tempo_seen:
                    tempo = int.from_bytes(data[body:body + 3], "big")
                    tempo_seen = True
                pos = body + length
                continue
            if byte in (0xF0, 0xF7):
                length, body = _varlen(data, pos + 1)
                pos = body + length
                continue
            if byte & 0x80:
                status = byte
                pos += 1
            kind, channel = status & 0xF0, status & 0x0F
            if kind in (0x80, 0x90):
                note, velocity = data[pos], data[pos + 1]
                pos += 2
                key = (channel, note)
                if kind == 0x90 and velocity > 0:
                    open_notes[key] = tick
                elif key in open_notes:
                    start = open_notes.pop(key)
                    channels.setdefault(channel, []).append((start, tick - start, note))
            elif kind in (0xA0, 0xB0, 0xE0):
                pos += 2
            elif kind in (0xC0, 0xD0):
                pos += 1
            else:
                raise CompanionError("unsupported MIDI event")
        pos = end
    return {"tpq": division, "tempo": tempo, "channels": channels}


def _fold(note: int, low: int, high: int) -> int:
    while note > high:
        note -= 12
    while note < low:
        note += 12
    return note


def _drum_kind(note: int) -> int:
    if note in (35, 36):
        return 0
    if note in (38, 39, 40, 41, 43, 45, 47, 48, 50):
        return 1
    return 2


def chart_from_midi(data: bytes, title: str = "Custom", level: int = 2, lead_in: int = 16,
                    bpm: int | None = None, min_gap_ms: int = 170) -> dict[str, Any]:
    midi = parse_midi(data)
    tpq = midi["tpq"]
    real_bpm = 60_000_000 / midi["tempo"]
    while real_bpm < 60:
        real_bpm *= 2
    while real_bpm > 240:
        real_bpm /= 2
    bpm = int(round(bpm or real_bpm))
    step_ticks = tpq / 4

    def to_step(ticks: int) -> int:
        return int(round(ticks / step_ticks))

    melodic = {c: n for c, n in midi["channels"].items() if c != 9 and len(n) >= 4}
    if not melodic:
        raise CompanionError("the MIDI file has no melodic channel with enough notes")
    by_pitch = sorted(melodic, key=lambda c: sum(n[2] for n in melodic[c]) / len(melodic[c]))
    lead_channel = by_pitch[-1]
    bass_channel = by_pitch[0] if len(by_pitch) > 1 else None

    def monophonic(notes: list[tuple[int, int, int]], top: bool) -> list[tuple[int, int, int]]:
        picked: dict[int, tuple[int, int, int]] = {}
        for start, length, note in notes:
            step = to_step(start)
            current = picked.get(step)
            if current is None or (note > current[2] if top else note < current[2]):
                picked[step] = (step, max(1, min(64, to_step(length))), note)
        return [picked[key] for key in sorted(picked)]

    lead = monophonic(melodic[lead_channel], top=True)
    bass = monophonic(melodic[bass_channel], top=False) if bass_channel is not None else []
    drums: list[tuple[int, int]] = []
    for start, _length, note in midi["channels"].get(9, []):
        drums.append((to_step(start), _drum_kind(note)))

    events: list[tuple[int, int, int, int, int]] = []
    for step, length, note in lead:
        events.append((step + lead_in, VOICE_LEAD, _fold(note, 48, 96), length, 90))
    for step, length, note in bass:
        events.append((step + lead_in, VOICE_BASS, _fold(note, 24, 60), length, 100))
    seen_drums = set()
    for step, kind in drums:
        if (step, kind) not in seen_drums:
            seen_drums.add((step, kind))
            events.append((step + lead_in, VOICE_DRUM, kind, 1, 100))
    # Trim to the device limit: hats first, then kicks, then bass.
    if len(events) > MAX_EVENTS:
        def priority(event: tuple[int, int, int, int, int]) -> int:
            if event[1] == VOICE_DRUM:
                return 0 if event[2] == 2 else 2
            return 1 if event[1] == VOICE_BASS else 3
        keep = sorted(events, key=lambda e: (-priority(e), e[0]))[:MAX_EVENTS]
        events = keep
    events.sort(key=lambda e: (e[0], e[1]))

    # Chart: lead onsets, filled by snare/kick hits, thinned to a playable density.
    step_ms = 15000 / bpm
    min_gap = max(1, int(-(-min_gap_ms // step_ms)))
    pitches = sorted(note for _s, _l, note in lead) or [60]
    median = pitches[len(pitches) // 2]
    candidates: list[tuple[int, int, int, bool]] = []  # step, priority, lane, big
    for step, length, note in lead:
        candidates.append((step, 3, 1 if note >= median else 0, length >= 4))
    for step, kind in drums:
        if kind == 1:
            candidates.append((step, 2, 1, False))
        elif kind == 0:
            candidates.append((step, 1, 0, False))
    candidates.sort(key=lambda c: (c[0], -c[1]))
    notes: list[tuple[int, int]] = []
    last_step = -10_000
    same_lane = 0
    last_lane = -1
    for step, _prio, lane, big in candidates:
        if step - last_step < min_gap:
            continue
        if lane == last_lane:
            same_lane += 1
            if same_lane >= 4:
                lane = 1 - lane
                same_lane = 0
        else:
            same_lane = 0
        last_lane = lane
        last_step = step
        notes.append((step + lead_in, (NOTE_BIG_AIR if lane else NOTE_BIG_GROUND) if big else (NOTE_AIR if lane else NOTE_GROUND)))
    if not notes:
        raise CompanionError("no playable notes found in the MIDI file")
    last = max([n[0] for n in notes] + [e[0] for e in events])
    return {
        "title": "".join(ch if 32 <= ord(ch) < 127 else "?" for ch in title)[:12] or "Custom",
        "bpm": bpm,
        "level": level,
        "steps": last + 16,
        "notes": [value for pair in notes for value in pair],
        "events": [value for event in events for value in event],
    }


def demo_chart() -> dict[str, Any]:
    """A short C-major arpeggio song, handy to test the pipeline without MIDI."""
    melody = [60, 64, 67, 72, 67, 64, 60, 64, 62, 65, 69, 74, 69, 65, 62, 65]
    events: list[int] = []
    notes: list[int] = []
    for bar in range(8):
        base = 16 + bar * 16
        for i in range(0, 16, 2):
            note = melody[(i // 2 + bar) % len(melody)]
            events += [base + i, VOICE_LEAD, note, 2, 90]
            notes += [base + i, NOTE_AIR if note >= 65 else NOTE_GROUND]
        events += [base, VOICE_BASS, 36 if bar % 2 == 0 else 43, 8, 100]
        events += [base + 8, VOICE_BASS, 36 if bar % 2 == 0 else 43, 8, 100]
        for i in range(0, 16, 4):
            events += [base + i, VOICE_DRUM, 0 if i % 8 == 0 else 1, 1, 100]
    return {"title": "PC Demo", "bpm": 120, "level": 1, "steps": 16 + 8 * 16 + 16, "notes": notes, "events": events}


# -- commands ---------------------------------------------------------------------


def resolve_board(args: argparse.Namespace) -> Board:
    config = load_config()
    host = args.host or os.environ.get("EDGITALK_HOST") or config.get("host")
    token = args.token or os.environ.get("EDGITALK_TOKEN") or config.get("token", "")
    if not host:
        cards = discover()
        if len(cards) == 1:
            host = cards[0]["ip"]
            print(f"found {cards[0].get('name', 'board')} at {host}")
        elif not cards:
            raise CompanionError("no board found; pass --host or run `edgitalk.py pair <ip> <code>`")
        else:
            raise CompanionError("several boards found; pass --host: " + ", ".join(c["ip"] for c in cards))
    return Board(host, token, port=args.port)


def cmd_discover(args: argparse.Namespace) -> int:
    cards = discover(timeout=args.timeout)
    if not cards:
        print("no board answered. On this Wi-Fi the broadcast often never arrives;")
        print("read the IP on the board's Settings page and run: edgitalk.py pair <ip> <code>")
        return 1
    for card in cards:
        print(f"{card['ip']:<16} {card.get('name', '')}  port {card.get('port', 80)}")
    return 0


def cmd_pair(args: argparse.Namespace) -> int:
    board = Board(args.ip, args.code, port=args.port)
    info = board.status()
    save_config({"host": args.ip, "token": args.code})
    print(f"paired with {info.get('name', args.ip)} ({args.ip}); saved to {CONFIG_PATH}")
    return 0


def cmd_status(args: argparse.Namespace) -> int:
    info = resolve_board(args).status()
    song = info.get("song", {})
    print(f"name     {info.get('name')}")
    print(f"address  {info.get('ip') or '(hotspot mode)'}")
    print(f"song     {'custom song stored, %d bytes' % song.get('size', 0) if song.get('present') else 'none'}")
    print(f"playing  {'yes' if info.get('playing') else 'no'}")
    return 0


def cmd_scores(args: argparse.Namespace) -> int:
    data = resolve_board(args).scores()
    names = ["Sunny Steps", "Neon Pulse", "Cat Rush", "Custom"]
    for slot, best in enumerate(data.get("best", [])[:4]):
        rank = data.get("rank", "")[slot:slot + 1] or "-"
        print(f"{names[slot]:<12} {best:>8}  {rank}")
    return 0


def _load_chart(path: str) -> dict[str, Any]:
    try:
        with open(path, "r", encoding="utf-8") as handle:
            chart = json.load(handle)
    except (OSError, ValueError) as error:
        raise CompanionError(f"cannot read {path}: {error}") from None
    problems = validate_chart(chart)
    if problems:
        raise CompanionError(f"{path} is not a valid chart:\n  - " + "\n  - ".join(problems))
    return chart


def player_title(text: str) -> str:
    """ASCII title the home bar can draw, at most 16 characters."""
    cleaned = "".join(ch if ch.isalnum() or ch in " -_" else "" for ch in text).strip()
    return (cleaned or "Track")[:16]


def is_player_wav(data: bytes) -> bool:
    """True for mono 16-bit PCM at a rate the board already plays."""
    if len(data) < 44 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        return False
    offset = 12
    fmt = None
    while offset + 8 <= len(data):
        kind = data[offset:offset + 4]
        size = struct.unpack_from("<I", data, offset + 4)[0]
        start = offset + 8
        if start + size > len(data):
            return False
        if kind == b"fmt " and size >= 16:
            fmt = data[start:start + 16]
        elif kind == b"data":
            break
        offset = start + size + (size & 1)
    if fmt is None:
        return False
    audio_format, channels = struct.unpack_from("<HH", fmt, 0)
    rate = struct.unpack_from("<I", fmt, 4)[0]
    bits = struct.unpack_from("<H", fmt, 14)[0]
    return audio_format == 1 and channels == 1 and bits == 16 and rate in (16000, 24000, 48000, 96000)


def prepare_player_wav(raw: bytes, filename: str, title: str = "") -> tuple[bytes, str]:
    """Return a small mono MP3 the home player can store, plus a short ASCII title.

    /flash is 1 MB. A 16 kHz WAV of a normal song does not fit, so the file is
    packed down until it does.
    """
    name = player_title(title or os.path.splitext(os.path.basename(filename or "Track"))[0])
    packed = _ffmpeg_player_mp3(raw, filename)
    if len(packed) > PLAYER_WAV_LIMIT:
        raise CompanionError("板子的 flash 只有 1MB，这首压完还是放不下。")
    return packed, name


def _ffmpeg_player_mp3(raw: bytes, filename: str) -> bytes:
    suffix = os.path.splitext(filename or "")[1] or ".audio"
    src = ""
    best = b""
    try:
        with tempfile.NamedTemporaryFile(suffix=suffix, delete=False) as handle:
            handle.write(raw)
            src = handle.name
        for bitrate in ("16k", "12k", "8k"):
            dst = src + ".mp3"
            try:
                subprocess.run(
                    ["ffmpeg", "-y", "-i", src, "-ac", "1", "-ar", "16000",
                     "-codec:a", "libmp3lame", "-b:a", bitrate, "-f", "mp3", dst],
                    check=True, capture_output=True,
                )
            except FileNotFoundError:
                raise CompanionError("这台电脑没有 ffmpeg，转不了这首歌。") from None
            except subprocess.CalledProcessError as error:
                detail = (error.stderr or b"").decode("utf-8", "replace").strip().splitlines()
                tail = detail[-1] if detail else "ffmpeg 失败"
                raise CompanionError(f"转不成播放器能放的文件：{tail}") from None
            with open(dst, "rb") as handle:
                best = handle.read()
            try:
                os.unlink(dst)
            except OSError:
                pass
            if len(best) <= PLAYER_WAV_LIMIT:
                return best
    finally:
        if src:
            try:
                os.unlink(src)
            except OSError:
                pass
    return best


def cmd_push(args: argparse.Namespace) -> int:
    chart = _load_chart(args.chart)
    payload = encode_chart(chart)
    board = resolve_board(args)
    if board.status().get("playing"):
        raise CompanionError("a song is playing on the board; finish or exit it first")
    result = board.push(payload)
    print(f"uploaded {result.get('size', len(payload))} bytes: {chart.get('title', 'Custom')} "
          f"({chart['bpm']} BPM, {len(chart['notes']) // 2} notes)")
    print('On the board: SONGS -> the teal "PC" card.')
    return 0


def cmd_music(args: argparse.Namespace) -> int:
    """Send an audio file to the home player over Bluetooth."""
    import ble

    try:
        with open(args.file, "rb") as handle:
            raw = handle.read()
    except OSError as error:
        raise CompanionError(f"cannot read {args.file}: {error}") from None
    config = load_config()
    token = args.token or os.environ.get("EDGITALK_TOKEN") or config.get("token", "")
    wav, title = prepare_player_wav(raw, args.file, args.title or "")
    if len(wav) > PLAYER_WAV_LIMIT:
        raise CompanionError(f"player file is {len(wav)} bytes; limit is {PLAYER_WAV_LIMIT}")
    mac = ble.find_edgitalk()
    if not mac:
        raise CompanionError("EdgiTalk is not advertising")
    ble.push_music(mac, wav, str(token), title)
    print(f"sent {len(wav)} bytes to the home player as {title}")
    return 0


def cmd_pull(args: argparse.Namespace) -> int:
    data = resolve_board(args).pull()
    if args.output:
        with open(args.output, "wb") as handle:
            handle.write(data)
        print(f"saved {len(data)} bytes to {args.output}")
    else:
        sys.stdout.write(data.decode("utf-8", "replace") + "\n")
    return 0


def cmd_clear(args: argparse.Namespace) -> int:
    resolve_board(args).clear()
    print("custom song removed")
    return 0


def cmd_make_chart(args: argparse.Namespace) -> int:
    with open(args.midi, "rb") as handle:
        data = handle.read()
    title = args.title or os.path.splitext(os.path.basename(args.midi))[0]
    chart = chart_from_midi(data, title=title, level=args.level, bpm=args.bpm, lead_in=args.lead_in)
    problems = validate_chart(chart)
    if problems:
        raise CompanionError("generated chart is invalid: " + "; ".join(problems))
    payload = encode_chart(chart)
    with open(args.output, "wb") as handle:
        handle.write(payload)
    print(f"{args.output}: {chart['title']}, {chart['bpm']} BPM, {len(chart['notes']) // 2} notes, "
          f"{len(chart['events']) // 5} audio events, {len(payload)} bytes")
    return 0


def cmd_demo(args: argparse.Namespace) -> int:
    with open(args.output, "wb") as handle:
        handle.write(encode_chart(demo_chart()))
    print(f"wrote {args.output}")
    return 0


def cmd_validate(args: argparse.Namespace) -> int:
    chart = _load_chart(args.chart)
    print(f"ok: {chart.get('title', 'Custom')}, {chart['bpm']} BPM, {len(chart['notes']) // 2} notes, "
          f"{len(chart['events']) // 5} events")
    return 0


def cmd_stats(args: argparse.Namespace) -> int:
    import stats as pcstats

    board = resolve_board(args)
    sampler = pcstats.Sampler()
    time.sleep(0.4)  # the first CPU reading needs a baseline
    while True:
        payload = pcstats.collect(sampler, host=args.name, include_time=not args.no_time)
        board.push_stats(payload)
        temp = "n/a" if payload["temp"] < 0 else f"{payload['temp']}C"
        print(f"cpu {payload['cpu']}%  ram {payload['ramPct']}% ({payload['ramUsedMb']}/{payload['ramTotalMb']} MB)  "
              f"disk {payload['diskPct']}% ({payload['diskUsedGb']}/{payload['diskTotalGb']} GB)  temp {temp}")
        if not args.watch:
            return 0
        time.sleep(max(1.0, args.interval))


def cmd_console(args: argparse.Namespace) -> int:
    import console

    return console.run_console(
        host=args.host or "", token=args.token or "", port=args.port,
        open_browser=not args.no_browser, listen_port=args.listen)


# -- mock board ---------------------------------------------------------------------


def run_mock(host: str = "127.0.0.1", port: int = 8080, udp_port: int = DISCOVERY_PORT,
             token: str = "123456", ready: Any = None) -> None:
    """A stand-in for the board's HTTP + discovery services (mirrors pocketjs_wifi.c)."""
    import http.server
    import threading

    state: dict[str, Any] = {"song": None, "failures": 0, "playing": False, "pc": None, "pc_time": 0.0}

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_args: Any) -> None:
            pass

        def _json(self, code: int, body: Any) -> None:
            data = json.dumps(body).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def _auth(self) -> bool:
            if state["failures"] >= 5:
                self._json(429, {"ok": False, "error": "locked, retry later"})
                return False
            if self.headers.get("X-Token") == token:
                state["failures"] = 0
                return True
            state["failures"] += 1
            self._json(401, {"ok": False, "error": "bad token"})
            return False

        def do_GET(self) -> None:
            if self.path == "/api/status":
                song = state["song"]
                self._json(200, {"device": "edgi-talk", "app": "beat-dash", "api": 1, "name": "EdgiTalk-MOCK",
                                 "ip": host, "ap": False, "playing": state["playing"],
                                 "song": {"present": song is not None, "size": len(song or b""), "limit": SONG_LIMIT}})
            elif self.path in ("/api/scores", "/api/pc", "/api/song") and not self._auth():
                return
            elif self.path == "/api/scores":
                self._json(200, {"best": [0, 12840, 76749, 0, 0, 0, 0, 0], "rank": "-AS-----"})
            elif self.path == "/api/pc":
                pc = state["pc"]
                if pc is None:
                    self._json(200, {"seen": False, "ageMs": -1})
                else:
                    self._json(200, dict(pc, seen=True, ageMs=int((time.monotonic() - state["pc_time"]) * 1000)))
            elif self.path == "/api/song":
                if state["song"] is None:
                    self._json(404, {"ok": False, "error": "no custom song"})
                else:
                    self.send_response(200)
                    self.send_header("Content-Length", str(len(state["song"])))
                    self.end_headers()
                    self.wfile.write(state["song"])
            else:
                self._json(404, {"ok": False, "error": "unknown endpoint"})

        def do_PUT(self) -> None:
            if self.path == "/api/pc":
                if not self._auth():
                    return
                length = int(self.headers.get("Content-Length") or 0)
                if length <= 0 or length > 512:
                    return self._json(413, {"ok": False, "error": "stats too large"})
                try:
                    data = json.loads(self.rfile.read(length))
                except ValueError:
                    return self._json(400, {"ok": False, "error": "no cpu value"})
                if not isinstance(data, dict) or not _is_int(data.get("cpu")):
                    return self._json(400, {"ok": False, "error": "no cpu value"})
                state["pc"] = data
                state["pc_time"] = time.monotonic()
                return self._json(200, {"ok": True})
            if self.path != "/api/song":
                return self._json(404, {"ok": False, "error": "unknown endpoint"})
            if not self._auth():
                return
            length = int(self.headers.get("Content-Length") or 0)
            if length < 8 or length > SONG_LIMIT:
                return self._json(413, {"ok": False, "error": "song size out of range"})
            if state["playing"]:
                return self._json(409, {"ok": False, "error": "game is running"})
            body = self.rfile.read(length).strip()
            try:
                chart = json.loads(body)
            except ValueError:
                return self._json(400, {"ok": False, "error": "not a song object"})
            problems = validate_chart(chart)
            if problems:
                return self._json(400, {"ok": False, "error": problems[0]})
            body = json.dumps(chart, separators=(",", ":")).encode("utf-8")
            state["song"] = body
            self._json(200, {"ok": True, "size": length})

        def do_DELETE(self) -> None:
            if self.path != "/api/song":
                return self._json(404, {"ok": False, "error": "unknown endpoint"})
            if self._auth():
                state["song"] = None
                self._json(200, {"ok": True})

    def discovery() -> None:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("0.0.0.0", udp_port))
        while True:
            data, address = sock.recvfrom(64)
            if data.startswith(b"EDGI?"):
                sock.sendto(json.dumps({"device": "edgi-talk", "name": "EdgiTalk-MOCK", "port": port, "api": 1}).encode(), address)

    server = http.server.ThreadingHTTPServer((host, port), Handler)
    threading.Thread(target=discovery, daemon=True).start()
    if ready is not None:
        ready.state = state
        ready.server = server
        ready.set()
    print(f"mock board on http://{host}:{port}  pairing code {token}  (UDP discovery {udp_port})")
    server.serve_forever()


def cmd_mock(args: argparse.Namespace) -> int:
    run_mock(port=args.port if args.port != HTTP_PORT else 8080)
    return 0


# -- entry point --------------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="edgitalk", description="Edgi Talk companion for Beat Dash")
    parser.add_argument("--host", help="board address (default: saved pairing or auto-discovery)")
    parser.add_argument("--token", help="pairing code shown in the device Settings")
    parser.add_argument("--port", type=int, default=HTTP_PORT, help="HTTP port (default 80)")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("discover", help="find boards on the LAN")
    p.add_argument("--timeout", type=float, default=1.5)
    p.set_defaults(func=cmd_discover)

    p = sub.add_parser("pair", help="save host and pairing code")
    p.add_argument("ip")
    p.add_argument("code")
    p.set_defaults(func=cmd_pair)

    sub.add_parser("status", help="show device state").set_defaults(func=cmd_status)
    sub.add_parser("scores", help="show best scores").set_defaults(func=cmd_scores)

    p = sub.add_parser("push", help="upload a custom song")
    p.add_argument("chart")
    p.set_defaults(func=cmd_push)

    p = sub.add_parser("music", help="send an audio file to the home player over Bluetooth")
    p.add_argument("file")
    p.add_argument("--title", default="", help="name shown on the home bar (ASCII, 16 chars)")
    p.set_defaults(func=cmd_music)

    p = sub.add_parser("pull", help="download the stored custom song")
    p.add_argument("output", nargs="?")
    p.set_defaults(func=cmd_pull)

    sub.add_parser("clear", help="delete the custom song").set_defaults(func=cmd_clear)

    p = sub.add_parser("make-chart", help="convert a MIDI file into a chart")
    p.add_argument("midi")
    p.add_argument("-o", "--output", required=True)
    p.add_argument("--title")
    p.add_argument("--level", type=int, default=2, choices=(1, 2, 3))
    p.add_argument("--bpm", type=int)
    p.add_argument("--lead-in", type=int, default=16, help="silent steps before the first note (default 16)")
    p.set_defaults(func=cmd_make_chart)

    p = sub.add_parser("demo", help="write a small example chart")
    p.add_argument("-o", "--output", default="demo-chart.json")
    p.set_defaults(func=cmd_demo)

    p = sub.add_parser("validate", help="check a chart file")
    p.add_argument("chart")
    p.set_defaults(func=cmd_validate)

    p = sub.add_parser("stats", help="send this PC's stats to the board's PC monitor page")
    p.add_argument("--watch", action="store_true", help="keep sending until interrupted")
    p.add_argument("--interval", type=float, default=2.0, help="seconds between packets with --watch (default 2)")
    p.add_argument("--name", help="host name to show (default: this machine's name)")
    p.add_argument("--no-time", action="store_true", help="do not send the PC clock")
    p.set_defaults(func=cmd_stats)

    page = argparse.ArgumentParser(add_help=False)
    page.add_argument("--listen", type=int, default=0, help="local page port (default: a free port)")
    page.add_argument("--no-browser", action="store_true", help="do not open a browser window")
    sub.add_parser("console", parents=[page], help="open the console in a browser").set_defaults(func=cmd_console)
    sub.add_parser("gui", parents=[page], help="same as console").set_defaults(func=cmd_console)
    sub.add_parser("mock", help="run a fake board for testing").set_defaults(func=cmd_mock)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except CompanionError as error:
        print(f"edgitalk: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    # `console` imports this module by name; make it reuse this copy so exceptions line up.
    sys.modules.setdefault("edgitalk", sys.modules[__name__])
    sys.exit(main())
