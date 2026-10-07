"""Tests for the companion: chart conversion, protocol and auth against the mock board.

    python3 -m unittest -v tools/edgitalk-companion/test_edgitalk.py
"""

import json
import os
import sys
import threading
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import edgitalk  # noqa: E402
import stats as pcstats  # noqa: E402
import console  # noqa: E402
import ipaddress  # noqa: E402
import urllib.error  # noqa: E402
import urllib.request  # noqa: E402

HTTP_PORT = 18081
UDP_PORT = 47811


def vlq(value: int) -> bytes:
    out = [value & 0x7F]
    value >>= 7
    while value:
        out.append((value & 0x7F) | 0x80)
        value >>= 7
    return bytes(reversed(out))


def make_midi() -> bytes:
    """Two melodic channels plus drums, 120 BPM, 480 ticks per quarter."""
    tpq = 480
    events = []  # (tick, order, bytes)
    events.append((0, 0, b"\xff\x51\x03" + (500000).to_bytes(3, "big")))
    for bar in range(4):
        for i in range(8):
            start = (bar * 8 + i) * tpq // 2
            lead = 72 + [0, 4, 7, 12, 7, 4, 0, 2][i]
            events.append((start, 1, bytes([0x90, lead, 90])))
            events.append((start + tpq // 2 - 10, 0, bytes([0x80, lead, 0])))
            bass = 40 if i < 4 else 43
            events.append((start, 1, bytes([0x91, bass, 90])))
            events.append((start + tpq // 2 - 10, 0, bytes([0x81, bass, 0])))
            events.append((start, 1, bytes([0x99, 36 if i % 2 == 0 else 38, 100])))
            events.append((start + 20, 0, bytes([0x89, 36 if i % 2 == 0 else 38, 0])))
    events.sort(key=lambda e: (e[0], e[1]))
    track = bytearray()
    last = 0
    for tick, _order, payload in events:
        track += vlq(tick - last) + payload
        last = tick
    track += b"\x00\xff\x2f\x00"
    return b"MThd" + (6).to_bytes(4, "big") + (0).to_bytes(2, "big") + (1).to_bytes(2, "big") + tpq.to_bytes(2, "big") + \
        b"MTrk" + len(track).to_bytes(4, "big") + bytes(track)


class Ready(threading.Event):
    state = None
    server = None


class CompanionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ready = Ready()
        threading.Thread(
            target=edgitalk.run_mock,
            kwargs={"port": HTTP_PORT, "udp_port": UDP_PORT, "token": "123456", "ready": cls.ready},
            daemon=True,
        ).start()
        assert cls.ready.wait(5)

    @classmethod
    def tearDownClass(cls):
        cls.ready.server.shutdown()

    def setUp(self):
        self.ready.state.update(song=None, failures=0, playing=False)
        self.board = edgitalk.Board("127.0.0.1", "123456", port=HTTP_PORT)

    def test_discovery(self):
        cards = edgitalk.discover(timeout=1.0, port=UDP_PORT, targets=["127.0.0.1"])
        self.assertEqual(len(cards), 1)
        self.assertEqual(cards[0]["ip"], "127.0.0.1")
        self.assertEqual(cards[0]["port"], HTTP_PORT)

    def test_status_and_scores(self):
        info = self.board.status()
        self.assertEqual(info["device"], "edgi-talk")
        self.assertFalse(info["song"]["present"])
        self.assertEqual(len(self.board.scores()["best"]), 8)

    def test_push_pull_clear(self):
        payload = edgitalk.encode_chart(edgitalk.demo_chart())
        self.assertTrue(self.board.push(payload)["ok"])
        self.assertTrue(self.board.status()["song"]["present"])
        self.assertEqual(self.board.pull(), payload)
        self.assertTrue(self.board.clear()["ok"])
        with self.assertRaises(edgitalk.CompanionError):
            self.board.pull()

    def test_wrong_token_and_lockout(self):
        bad = edgitalk.Board("127.0.0.1", "000000", port=HTTP_PORT)
        payload = edgitalk.encode_chart(edgitalk.demo_chart())
        for _ in range(5):
            with self.assertRaisesRegex(edgitalk.CompanionError, "401"):
                bad.push(payload)
        with self.assertRaisesRegex(edgitalk.CompanionError, "429"):
            self.board.push(payload)  # even the right code is refused while locked

    def test_rejects_non_object_and_busy(self):
        with self.assertRaisesRegex(edgitalk.CompanionError, "400"):
            self.board.push(b"[1,2,3,4,5,6,7,8,9]")
        self.ready.state["playing"] = True
        with self.assertRaisesRegex(edgitalk.CompanionError, "409"):
            self.board.push(edgitalk.encode_chart(edgitalk.demo_chart()))

    def test_demo_chart_is_valid(self):
        self.assertEqual(edgitalk.validate_chart(edgitalk.demo_chart()), [])

    def test_midi_conversion(self):
        chart = edgitalk.chart_from_midi(make_midi(), title="Test Tune \u00e9")
        self.assertEqual(edgitalk.validate_chart(chart), [])
        self.assertEqual(chart["bpm"], 120)
        self.assertTrue(chart["title"].isascii())
        notes = chart["notes"]
        steps = notes[0::2]
        self.assertEqual(steps, sorted(steps))
        self.assertGreaterEqual(steps[0], 16)  # lead-in
        gap_ms = min(b - a for a, b in zip(steps, steps[1:])) * 15000 / chart["bpm"]
        self.assertGreaterEqual(gap_ms, 170)
        voices = {chart["events"][i] for i in range(1, len(chart["events"]), 5)}
        self.assertEqual(voices, {0, 1, 2})
        json.dumps(chart)

    def test_pc_stats_round_trip(self):
        sampler = pcstats.Sampler()
        payload = pcstats.collect(sampler, host="a-very-long-host-name-\u00e9")
        self.assertLessEqual(len(payload["host"]), 16)
        self.assertTrue(payload["host"].isascii())
        for key in ("cpu", "ramPct", "diskPct"):
            self.assertTrue(0 <= payload[key] <= 100, key)
        self.assertTrue(-1 <= payload["temp"] <= 150)
        self.assertGreaterEqual(payload["t"], 1704067200)
        # The board parses this with a flat key scan and a 512 byte body limit.
        self.assertLess(len(json.dumps(payload, separators=(",", ":"))), 512)
        self.assertTrue(self.board.push_stats(payload)["ok"])
        echo = self.board.pc()
        self.assertTrue(echo["seen"])
        self.assertEqual(echo["cpu"], payload["cpu"])
        self.assertLess(echo["ageMs"], 5000)

    def test_pc_stats_need_token_and_cpu(self):
        self.assertFalse(self.board.pc()["seen"])
        anonymous = edgitalk.Board("127.0.0.1", "", port=HTTP_PORT)
        with self.assertRaises(edgitalk.CompanionError):
            anonymous.push_stats({"cpu": 1})
        with self.assertRaisesRegex(edgitalk.CompanionError, "400"):
            self.board.push_stats({"ramPct": 5})

    def test_time_fields_are_unix_seconds_and_minutes(self):
        payload = pcstats.collect(pcstats.Sampler(), include_time=True)
        self.assertAlmostEqual(payload["t"], time.time(), delta=5)
        self.assertTrue(-720 <= payload["tz"] <= 840)
        self.assertNotIn("t", pcstats.collect(pcstats.Sampler(), include_time=False))

    def test_validate_reports_problems(self):
        self.assertTrue(edgitalk.validate_chart({"bpm": 20, "notes": [], "events": [1, 2]}))


class ConsoleApiTests(unittest.TestCase):
    def setUp(self):
        self.app = console.Console(
            config={}, discover=lambda: [], save=lambda _config: None, autosearch=False,
        )
        self.server = console.serve(self.app, open_browser=False)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base = f"http://127.0.0.1:{self.server.server_port}"
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def tearDown(self):
        self.app.close()
        self.server.shutdown()

    def post(self, path, body):
        request = urllib.request.Request(
            self.base + path, data=json.dumps(body).encode(), method="POST",
            headers={"Content-Type": "application/json"},
        )
        try:
            with self.opener.open(request, timeout=3) as response:
                return response.status, json.load(response)
        except urllib.error.HTTPError as error:
            return error.code, json.load(error)

    def test_connect_requires_the_pair_code(self):
        code, body = self.post("/api/local/connect", {})
        self.assertEqual(code, 400)
        self.assertIn("配对码", body["error"])

    def test_discover_miss_tells_you_the_board_was_not_found(self):
        code, body = self.post("/api/local/discover", {})
        self.assertEqual(code, 200)
        self.assertFalse(body["ok"])
        self.assertIn("没有找到板子", body["error"])

    def test_chart_preview(self):
        chart = edgitalk.demo_chart()
        snap = self.app.store_chart("demo.json", json.dumps(chart).encode(), "Demo", 1)
        self.assertIn("BPM", snap["chartNote"])
        self.assertIn("个音符", snap["chartNote"])

    def test_nearby_blocks_start_at_this_machine(self):
        mine = ipaddress.IPv4Address("10.31.0.38")
        network = ipaddress.ip_network("10.31.0.0/16")
        blocks = console.blocks_near(mine, network, limit=4)
        self.assertEqual(str(blocks[0]), "10.31.0.0/24")
        self.assertIn(ipaddress.ip_network("10.31.3.0/24"), blocks)


if __name__ == "__main__":
    unittest.main()
