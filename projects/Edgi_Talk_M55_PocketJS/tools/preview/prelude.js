// Scripted stand-in for the firmware's `__edgi` bridge (desktop preview only).
globalThis.__sim = {
  frame: 0,
  startFrame: -1,
  running: false,
  starts: [],
  stops: 0,
  sfx: [0, 0, 0],
  saves: [],
  offset: 0,
  volume: 50,
  musicState: 1,
  input: function () { return ""; },
  shot: function () { return ""; },
  report: function () { return ""; },
};

globalThis.__edgi = {
  status: function () {
    var wifi = globalThis.__wifi !== false;
    return {
      cpu: 23, ram: 41, fps: 29, flash: 18, temp: 265, tempSource: 1,
      wifi: wifi, ssid: "Srakoul-5G", ip: wifi ? "192.168.1.23" : "", token: "483920",
      ap: !wifi, apSsid: "EdgiTalk-A1B2C3", apPassword: "Edgi5F3A9C21",
      weather: "CLEAR", weatherTemp: 246, month: 9, day: 29,
    };
  },
  musicStatus: function () {
    return { track: "Midnight Radio", index: 0, count: 3, volume: __sim.volume, state: __sim.musicState, sd: true };
  },
  musicCommand: function (c) {
    if (c === 1) __sim.musicState = __sim.musicState === 1 ? 2 : 1;
    if (c === 4) __sim.volume -= 10;
    if (c === 5) __sim.volume += 10;
    return true;
  },
  gameStart: function (bpm, events) {
    __sim.running = true;
    __sim.startFrame = __sim.frame;
    __sim.starts.push({ bpm: bpm, events: events.length / 5, frame: __sim.frame });
    return true;
  },
  gameStop: function () {
    if (__sim.running) __sim.stops++;
    __sim.running = false;
  },
  gameClock: function () {
    return __sim.running ? (__sim.frame - __sim.startFrame) * 1000 / 30 : -1;
  },
  gameSfx: function (id) { __sim.sfx[id]++; },
  gameScores: function () { return { best: [0, 12840, 0, 0, 0, 0, 0, 0], rank: "-A-----" + "-" }; },
  gameSave: function (song, score, rank) { __sim.saves.push([song, score, rank]); return true; },
  gameOffset: function () { return __sim.offset; },
  gameSetOffset: function (ms) { __sim.offset = ms; },
  customSong: function () { return globalThis.__custom || ""; },
};
