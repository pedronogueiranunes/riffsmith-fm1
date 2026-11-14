#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Riffsmith contributors
"""Riffsmith: the built-in drum kit from public material only.

M-PYRE builds its HEAVY KIT from recordings that are not in the public tree (assets/mpyre-src). This script makes
the same set (PERC: the same GM notes, names and loudness rules as tools/prep_mpyre.py) from the CC0 kit in
assets/samples-cc0/KIT, plus the two effect lanes M-PYRE synthesises (riser, drop). With assets/mpyre-src present,
use tools/prep_mpyre.py instead.

  tools/prep_riffsmith_kit.py [OUT_DIR]     (default build/mpyre; needs numpy, scipy, ffmpeg)

Writes OUT_DIR/sets.json with the PERC set only; the guitar, palm-mute and bass samples are M-PYRE's sound pack
(soundpack.bin from its release, CC BY 4.0 History Of Sky), installed into the user slots by the editor.
"""
import base64
import json
import sys
from pathlib import Path

import numpy as np

SRC = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SRC / "tools"))
import prep_mpyre as mp  # noqa: E402  (load, k_loud, fx_sound, TR, KIT, loudness constants)

CC0 = SRC / "assets" / "samples-cc0" / "KIT"
# GM note -> (CC0 file, pitch shift in semitones, keep seconds, relative loudness dB as prep_mpyre.KIT)
MAP = {
    36: ("10_kick_BDrumNew_hit_v5", 0, 0.6, 0),
    38: ("11_snare_Snare2_HitSN_v7", 0, 0.9, -1),
    40: ("12_rim_sidestick", 0, 0.6, -9),
    42: ("14_hihat_HitC_v3", 0, 0.3, -11),
    46: ("15_ohat_HitO", 0, 1.5, -9),
    44: ("14_hihat_HitC_v3", -2, 0.35, -12),
    51: ("19_ride_susCymb2_stick_mf1", 0, 1.35, -13),
    53: ("20_cowbell_Cowbell1_Normal_v3", 0, 1.2, -12),
    48: ("17_tomhi_TomH_HitS_v4", 0, 1.0, -3.5),
    47: ("17_tomhi_TomH_HitS_v4", -4, 1.0, -3),
    43: ("16_tomlo_TomL_HitS_v4", 0, 1.1, -3),
    49: ("18_crash_susCymb2_hit_f1", 0, 1.4, -10),
    57: ("18_crash_susCymb2_hit_f1", 1.5, 1.4, -10),
    52: ("18_crash_susCymb2_hit_f1", -3, 1.35, -10),
}
NAMES = {gm: name for gm, name, _, _ in mp.KIT}


def shifted(x, semis):
    """pitch by resampling (a drum gets shorter as it goes up, as a smaller drum would)"""
    if not semis:
        return x
    r = 2 ** (semis / 12)
    idx = np.arange(0, len(x) - 1, r)
    return np.interp(idx, np.arange(len(x)), x)


def drums():
    out = []
    for gm, fn, keep, rel in mp.KIT:
        if fn.startswith("FX:"):
            out.append(dict(name=fn[3:].upper(), gm=gm, x=mp.fx_sound(fn[3:], keep), rel=rel))
            continue
        rev = fn.startswith("REV:")
        src = MAP[49 if rev else gm]
        x = shifted(mp.load(CC0 / f"{src[0]}.wav", mp.TR), src[1])
        on = max(0, int(np.argmax(np.abs(x) > np.abs(x).max() * 0.01)) - 22)
        x = x[on:on + int((1.6 if rev else keep) * mp.TR)].copy()
        fd = int(len(x) * 0.4)
        x[-fd:] *= np.linspace(1, 0, fd) ** 2
        if rev:
            x = x[::-1][-int(keep * mp.TR):].copy()
            x[:int(0.05 * mp.TR)] *= np.linspace(0, 1, int(0.05 * mp.TR))
        out.append(dict(name=NAMES[gm], gm=gm, x=x, rel=rel))
    gains = [10 ** ((mp.KICK_LUFS + d["rel"] - mp.k_loud(d["x"], mp.TR)) / 20) for d in out]
    fac = min((10 ** ((mp.KICK_PEAK if d["name"] in ("Kick", "Snare") else mp.PEAK) / 20)) / (np.abs(d["x"]).max() * g)
              for d, g in zip(out, gains))
    zones = []
    for d, g in zip(out, gains):
        pcm = np.clip(np.round(d["x"] * g * fac * 32767), -32768, 32767).astype("<i2")
        zones.append(dict(name=d["name"], root=d["gm"], lo=d["gm"], hi=d["gm"], loop=None, deemph=False,
                          pcm=base64.b64encode(pcm.tobytes()).decode()))
    return zones


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else SRC / "build" / "mpyre"
    out.mkdir(parents=True, exist_ok=True)
    perc = drums()
    (out / "sets.json").write_text(json.dumps({"PERC": perc}))
    n = sum(len(base64.b64decode(z["pcm"])) // 2 for z in perc)
    print(f"riffsmith: PERC {len(perc):2} zones, {n / mp.TR:5.2f} s (CC0 kit + synthesised FX)")


if __name__ == "__main__":
    main()
