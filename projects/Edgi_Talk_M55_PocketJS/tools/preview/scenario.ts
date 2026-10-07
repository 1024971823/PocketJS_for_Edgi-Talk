// Scripted play-through for the desktop preview harness.
import { BUILTIN_SONGS, stepMs } from "../../../../../pocketjs/apps/edgitalk-m55-smoke/game/songs.ts";

interface Sim {
  frame: number;
  startFrame: number;
  starts: unknown[];
  stops: number;
  sfx: number[];
  saves: unknown[];
  input: (n: number) => string;
  shot: (n: number) => string;
  report: () => string;
}

const g = globalThis as unknown as { __sim: Sim; __mode?: string; __song?: number };
const sim = g.__sim;
const mode = g.__mode ?? "perfect";
const songIndex = g.__song ?? 2;
const song = BUILTIN_SONGS[songIndex];

const NAV_TAPS: Record<number, [number, number]> = {
  12: [107, 141], // PLAY on home
  32: [12 + songIndex * 96 + 44, 130], // song card
  46: [200, 120], // tap to start
};
const SHOTS: Record<number, string> = { 10: "home", 30: "songs", 45: "ready" };

const unit = stepMs(song.bpm);
const START = 46;
const JITTER = [0, 0, 1, 0, -1, 0, 0, 1, 0, 0, -1, 0];
const noteTaps: Record<number, [number, number]> = {};
for (let i = 0; i < song.notes.length; i += 2) {
  const t = song.notes[i] * unit;
  const type = song.notes[i + 1];
  const frame = START + Math.round(t / (1000 / 30)) + JITTER[(i / 2) % JITTER.length];
  const air = type === 1 || type === 3;
  if (!(frame in noteTaps) && !((frame - 1) in noteTaps)) noteTaps[frame] = [200, air ? 84 : 156];
}

let lastTapFrame = -10;
let lastTap: [number, number] = [0, 0];
sim.input = (n: number): string => {
  const nav = NAV_TAPS[n];
  if (nav) {
    lastTapFrame = n;
    lastTap = nav;
  } else if (mode === "perfect" && sim.startFrame >= 0 && noteTaps[n]) {
    lastTapFrame = n;
    lastTap = noteTaps[n];
  }
  if (n - lastTapFrame >= 0 && n - lastTapFrame < 2) return `${lastTap[0]},${lastTap[1]}`;
  return "";
};

let resultShotAt = -1;
sim.shot = (n: number): string => {
  if (SHOTS[n]) return SHOTS[n];
  if (sim.startFrame >= 0) {
    const since = n - sim.startFrame;
    if (since === 80) return "play1";
    if (since === 260) return "play2";
    if (since === 700) return "play3";
  }
  if (sim.startFrame >= 0 && !(sim as unknown as { running?: boolean }).running && sim.stops > 0) {
    if (resultShotAt < 0) resultShotAt = n + 20;
    if (n === resultShotAt) return "result";
  }
  return "";
};

sim.report = (): string =>
  JSON.stringify({ song: song.title, mode, starts: sim.starts, stops: sim.stops, sfx: sim.sfx, saves: sim.saves, notes: song.notes.length / 2 });
