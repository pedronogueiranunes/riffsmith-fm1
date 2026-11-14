#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Riffsmith contributors
"""Riffsmith: render one short demo per style through the firmware's own DSP (build/host/mpyre_render), with the
synthesised GUITAR engine (no sound pack needed), a synth bass and the drum track.

  python3 tests/riffsmith_demos.py [OUT_DIR]      (after building build/host/mpyre_render)

Writes OUT_DIR/{indie,metal,reggae}.txt (the scripts) and .wav, and prints each render's peak and RMS.
"""
import subprocess
import sys
import wave
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
RENDER = ROOT / "build" / "host" / "mpyre_render"
ENGI_GUITAR, ENGI_ANALOG = 10, 0
P_PALM = 77
GUITAR = {"JANGLE": 0, "INDIE CRN": 1, "DREAM ARP": 2, "SYN CHUG": 3, "SYN DJENT": 4, "SYN LEAD": 5,
          "ONE DROP": 6, "DUB CHOP": 7, "SKA UP": 8, "STRING DI": 9}
SUB_BASS, MONO_BASS = 2, 18
KICK, SNARE, RIM, HAT, OHAT = 36, 38, 40, 42, 46


class Script:
    def __init__(self, bpm, bars):
        self.bpm, self.bars, self.lines = bpm, bars, []
        self.beat = 60.0 / bpm
        self.lines += [f"len {bars * 4 * self.beat + 1.5:.3f}", f"bpm {bpm}"]

    def t(self, bar, sixteenth):
        return (bar * 16 + sixteenth) * self.beat / 4

    def sound(self, track, engine, preset):
        self.lines.append(f"eng {track} {engine} {preset}")

    def note(self, track, start, dur, notes, vel=100):
        for n in notes:
            self.lines.append(f"on {start:.4f} {track} {n} {vel}")
            self.lines.append(f"off {start + dur:.4f} {track} {n}")

    def pset(self, time, track, param, value):
        self.lines.append(f"pset {time:.4f} {track} {param} {value}")

    def drum(self, time, note, vel=100):
        self.lines.append(f"drum {time:.4f} {note} {vel}")

    def text(self):
        return "\n".join(self.lines) + "\n"


def up(chord, k=12):
    return [n + k for n in chord]


def indie():
    s = Script(118, 4)
    s.sound(0, ENGI_GUITAR, GUITAR["JANGLE"])
    s.sound(1, ENGI_ANALOG, MONO_BASS)
    chords = [[43, 47, 50, 55, 59, 67], [50, 57, 62, 66], [40, 47, 52, 55, 59, 64], [48, 52, 55, 60, 64]]   # G D Em C
    roots = [31, 38, 28, 36]
    for bar, (ch, root) in enumerate(zip(chords, roots)):
        # D . D U . U D U, let ring (the guitar sounds an octave under the keys: TRN -12)
        for step, vel, dur in [(0, 110, 4), (4, 96, 2), (6, 80, 4), (10, 80, 2), (12, 96, 2), (14, 80, 2)]:
            s.note(0, s.t(bar, step), dur * s.beat / 4 * 0.95, up(ch), vel)
        for step in range(0, 16, 2):
            s.note(1, s.t(bar, step), s.beat / 2 * 0.8, [root + 12], 100)
        for step in range(16):
            if step in (0, 8):
                s.drum(s.t(bar, step), KICK, 110)
            if step in (4, 12):
                s.drum(s.t(bar, step), SNARE, 110)
            if step % 2 == 0:
                s.drum(s.t(bar, step), HAT, 70 if step % 4 else 90)
    return s


def metal():
    s = Script(150, 4)
    s.sound(0, ENGI_GUITAR, GUITAR["SYN CHUG"])
    s.sound(1, ENGI_ANALOG, SUB_BASS)
    root = 40                                          # E2 (keys an octave up)
    power = [root, root + 7, root + 12]
    for bar in range(4):
        hit = [root + 3, root + 5, root + 6, root + 5][bar]   # the accent chord climbs
        for step in range(16):
            tt = s.t(bar, step)
            if step in (0, 6, 12):                     # open power chords
                ch = power if step == 0 else [hit, hit + 7, hit + 12]
                s.pset(tt - 0.002, 0, P_PALM, 0)
                s.note(0, tt, s.beat / 4 * (2.4 if step < 12 else 1.8), up(ch), 120)
            elif step not in (1, 7, 13):               # palm-muted chugs on the low string
                s.pset(tt - 0.002, 0, P_PALM, 1)
                s.note(0, tt, s.beat / 4 * 0.85, up([root]), 112 if step % 2 == 0 else 96)
            if step % 2 == 0 or step in (13, 15):
                s.drum(tt, KICK, 115)
            if step in (4, 12):
                s.drum(tt, SNARE, 120)
            if step % 4 == 0:
                s.drum(tt, OHAT if step == 0 and bar == 0 else HAT, 90)
        s.note(1, s.t(bar, 0), s.beat * 3.8, [root - 12 + 12], 110)
    return s


def reggae():
    s = Script(76, 4)
    s.sound(0, ENGI_GUITAR, GUITAR["ONE DROP"])
    s.sound(1, ENGI_ANALOG, SUB_BASS)
    chords = [[57, 60, 64, 69], [55, 59, 62, 67]]      # Am, G: high, short chops
    bass = [[(0, 45, 3), (3, 45, 1), (6, 52, 2), (10, 48, 2), (12, 47, 2)],
            [(0, 43, 3), (3, 43, 1), (6, 50, 2), (10, 47, 2), (12, 45, 2)]]
    for bar in range(4):
        ch = chords[bar % 2]
        for step in (4, 12):                           # the skank on 2 and 4, choked
            s.note(0, s.t(bar, step), s.beat / 4 * 0.55, ch, 105)
        for step, n, ln in bass[bar % 2]:
            s.note(1, s.t(bar, step), ln * s.beat / 4 * 0.9, [n], 110)
        for step in range(16):
            tt = s.t(bar, step)
            if step == 8:                              # one drop: kick and rim together on 3
                s.drum(tt, KICK, 115)
                s.drum(tt, RIM, 110)
            if step % 2 == 0:
                s.drum(tt + (0.04 if step % 4 else 0), HAT, 60 if step % 4 else 80)   # (a little swing)
    return s


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build" / "demos"
    out.mkdir(parents=True, exist_ok=True)
    for name, make in (("indie", indie), ("metal", metal), ("reggae", reggae)):
        sc = out / f"{name}.txt"
        wav = out / f"{name}.wav"
        sc.write_text(make().text())
        subprocess.run([str(RENDER), str(sc), str(wav), "/nonexistent"], check=True, capture_output=True)
        with wave.open(str(wav)) as w:
            x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(float) / 32768
        peak, rms = np.abs(x).max(), np.sqrt((x ** 2).mean())
        print(f"{name:7} peak {peak:5.3f}  rms {20 * np.log10(rms + 1e-12):6.1f} dBFS  -> {wav}")


if __name__ == "__main__":
    main()
