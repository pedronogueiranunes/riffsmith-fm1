/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Riffsmith contributors */
/* RIFFSMITH: GUITAR, a synthesised electric guitar (no samples): one extended Karplus-Strong string per voice.
 *
 *   the pick     one period of string shape written into the string: the triangle of a string pulled aside at the
 *                pick position (PICK), plus low-passed noise for the pick's scrape (HARD, velocity); palm muting (PALM on
 *                the LIVE view / a step's ART, or MUTE) a softer, duller pick
 *   the string   a delay line of one period read with linear interpolation, a one-pole loop filter (BRIT: how much
 *                of the highs survive each trip), a loss per period from the decay time (SUST, shorter up the neck and
 *                much shorter palm muted), tuned for the filter's own delay so a chord stays in tune
 *   the fingers  key let go: the fretting finger lifts and damps the string (CHOP: from a natural ~250 ms to the
 *                ~25 ms choke of a reggae skank); the ADSR's release still applies on top
 *   the pickup   a DC blocker and TONE (the guitar's tone pot: a one-pole low-pass)
 *   DRIFT        each string a few cents off, as a real guitar's (chords shimmer)
 * Then the track's chain as for every sound: the pedals, the amp and its cabinet (amp.c), the effects.
 * The chord strum (P_STRUM), the chords (P_CHORD), bend, vibrato and palm mutes come from the riffbox around it.
 *
 * Memory: a string needs up to GT_LEN samples (F1, 43.7 Hz, the lowest pitch; a lower one plays at F1's length).
 * The FM-1 sounds NVOICE voices at once over all parts (voice.c), so GT_LINES lines are shared by every part
 * (16 KiB, in RAM: the engine arena of a part is ~2 KiB). A voice claims a line at its note-on; a line whose voice
 * has ended is free. Fixed point: the string in Q15 (int16 lines), coefficients Q15, the period Q16. */

#define GT_LINES NVOICE
#define GT_LEN 1024u
#define GT_MASK (GT_LEN - 1u)
static int16_t gt_line[GT_LINES][GT_LEN];
static const voice_t *gt_owner[GT_LINES];

/* voice state:
 *   s[0] line   s[1] loop filter   s[2] BRIT coefficient (Q15)   s[3] loss per period (Q15)
 *   s[4] DRIFT (signed Q15)   s[5] s[6] DC blocker x1, y1   s[7] bit 0 let go, bits 8.. quiet blocks
 *   ph[0] write position   ph[1] the loop filter's delay (Q16 samples)   ph[2] TONE's low-pass */

static uint32_t gt_period_q16(uint32_t inc)            /* one period in samples, Q16, from a phase increment */
{
    uint64_t p;
    if (inc < 16u)
        inc = 16u;
    p = ((uint64_t)1 << 48) / inc;
    return p > ((uint64_t)(GT_LEN - 3u) << 16) ? (GT_LEN - 3u) << 16 : (uint32_t)p;
}

/* the loss of one trip round the string for a decay of ms to -60 dB: 2^(-k), k = 9.966 * period / (44.1 * ms) */
static int32_t gt_loss(uint32_t period_q16, int32_t ms)
{
    uint32_t k, n, f, i, r, a, b;
    if (ms < 10)
        ms = 10;
    k = (uint32_t)((((uint64_t)period_q16 * 14810u) >> 16) / (uint32_t)ms);   /* octaves, Q16 */
    n = k >> 16;
    if (n > 14u)
        return 0;
    f = (k & 0xFFFFu) * 192u;                           /* the fraction in 1/192 octave, Q16 */
    i = f >> 16;
    r = f & 0xFFFFu;
    a = pow2_q16(-(int32_t)i);
    b = pow2_q16(-(int32_t)i - 1);
    a = (a - (uint32_t)(((uint64_t)(a - b) * r) >> 16)) >> n;   /* Q16 */
    a >>= 1;                                            /* Q15 */
    return a > 32700u ? 32700 : (int32_t)a;
}

/* the loop filter y += d (x - y) delays the string by (1 - d) / d samples at the low notes: Q16 */
static uint32_t gt_filter_delay(int32_t d)
{
    if (d < 1000)
        d = 1000;
    return (uint32_t)((((uint64_t)(32768 - d)) << 16) / (uint32_t)d);
}

static inline int16_t gt_sat16(int32_t x) { return (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x); }

static int32_t gt_palm(const track_t *t)               /* 0..127: PALM (on / off: a full palm mute) or MUTE */
{
    int32_t m = t->p[P_E3];
    return t->p[P_PALM] && m < 110 ? 110 : m;
}

static int32_t gt_sustain_ms(const track_t *t, uint32_t note, int32_t palm)
{
    /* SUST 0..127: 0.25 .. ~10 s at the low E (2^(SUST / 24)), shorter up the neck (x (82 Hz / f)^0.5), palm muted
     * down to ~150 ms */
    int32_t ms = (int32_t)((250u * pow2_q16(t->p[P_E2] * 8)) >> 16);
    int32_t up = (int32_t)note - 40;                    /* semitones above E2 */
    if (up > 0)
        ms = (int32_t)(((int64_t)ms * pow2_q16(-up * 8)) >> 16);   /* -1/2 octave per octave */
    ms = ms - (int32_t)(((int64_t)(ms - 150) * palm) / 127);
    return ms < 30 ? 30 : ms;
}

static void gt_note_on(track_t *t, voice_t *v)
{
    uint32_t li, best = 0, oldest = 0xFFFFFFFFu, i, n, w, pos, pick;
    int32_t palm = gt_palm(t), vel = v->vel, hard, lp = 0, peak = 1, mean = 0, amp, d;
    uint32_t seed = v->age * 2654435761u + v->note * 40503u + 1u;
    const uint32_t period = gt_period_q16(pitch_inc((uint32_t)v->note * 16u));

    /* a line: this voice's own (a retrigger), else a free one, else the one of the oldest voice */
    li = GT_LINES;
    for (i = 0; i < GT_LINES; i++)
        if (gt_owner[i] == v) {
            li = i;
            break;
        }
    if (li == GT_LINES)
        for (i = 0; i < GT_LINES; i++)
            if (!gt_owner[i] || !gt_owner[i]->active) {
                li = i;
                break;
            }
    if (li == GT_LINES) {
        for (i = 0; i < GT_LINES; i++)
            if (gt_owner[i]->age < oldest) {
                oldest = gt_owner[i]->age;
                best = i;
            }
        li = best;
    }
    gt_owner[li] = v;
    v->s[0] = (int32_t)li;

    /* the string: BRIT, darker palm muted */
    d = 9830 + (t->p[P_E1] * 21299) / 127;              /* 0.30 .. 0.95 */
    d -= (int32_t)(((int64_t)d * palm * 55) / (127 * 100));
    v->s[2] = d;
    v->ph[1] = gt_filter_delay(d);
    v->s[3] = gt_loss(period, gt_sustain_ms(t, v->note, palm));
    seed ^= seed >> 15;
    v->s[4] = (int32_t)(seed & 0xFFFFu) - 32768;        /* DRIFT's direction and amount for this note */
    v->s[1] = 0;
    v->s[5] = v->s[6] = 0;
    v->s[7] = 0;
    v->ph[2] = 0;

    /* the pick: one period of string shape behind the write position (where the string reads first): the triangle
     * of a string pulled aside at the pick point (harmonics falling as 1/n^2, the notches of the pick position its
     * own), plus filtered noise for the pick's scrape (HARD and velocity: more of it, brighter) */
    n = (period + 65535u) >> 16;
    if (n > GT_LEN - 2u)
        n = GT_LEN - 2u;
    w = v->ph[0] & GT_MASK;
    hard = 1500 + ((t->p[P_E5] * 9000) / 127) + vel * 40;   /* the scrape's low-pass, Q15 */
    hard -= (hard * palm * 60) / (127 * 100);
    for (i = 0; i < n; i++) {
        int32_t noise;
        seed = seed * 1664525u + 1013904223u;
        noise = (int32_t)(seed >> 17) - 16384;          /* +-16384 */
        lp += ((noise - lp) * hard) >> 15;
        gt_line[li][(w - n + i) & GT_MASK] = (int16_t)lp;
    }
    pick = (uint32_t)((n * (uint32_t)(6 + (t->p[P_E0] * 44) / 127)) / 100u);   /* PICK: 6 .. 50 % of the string */
    if (pick < 1u)
        pick = 1u;
    if (pick >= n)
        pick = n - 1u;
    for (i = 0; i < n; i++) {                           /* the scrape's level */
        int32_t x = gt_line[li][(w - n + i) & GT_MASK];
        x = x < 0 ? -x : x;
        if (x > peak)
            peak = x;
    }
    {
        /* the scrape's share of the pick, Q15: 0.1 .. ~0.45, less palm muted */
        int32_t nm = 3000 + (t->p[P_E5] * 8000) / 127 + vel * 30;
        nm -= (nm * palm) / 200;
        for (i = 0; i < n; i++) {
            int32_t tri = i < pick ? (int32_t)((i * 16384u) / pick) : (int32_t)(((n - i) * 16384u) / (n - pick));
            pos = (w - n + i) & GT_MASK;
            gt_line[li][pos] = gt_sat16(((tri * (32768 - nm)) >> 15) + (((gt_line[li][pos] * 16384) / peak) * nm >> 15));
        }
    }
    for (i = 0; i < n; i++)
        mean += gt_line[li][(w - n + i) & GT_MASK];
    mean /= (int32_t)n;
    peak = 1;
    for (i = 0; i < n; i++) {
        int32_t x = gt_line[li][(w - n + i) & GT_MASK] - mean;
        x = x < 0 ? -x : x;
        if (x > peak)
            peak = x;
    }
    amp = 9000 + vel * 120;                             /* the pick's peak: 9000 .. 24240 */
    if (palm)
        amp -= (amp * palm) / (127 * 3);
    for (i = 0; i < n; i++) {
        pos = (w - n + i) & GT_MASK;
        gt_line[li][pos] = gt_sat16(((gt_line[li][pos] - mean) * amp) / peak);
    }
}

static void gt_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    uint32_t li = (uint32_t)v->s[0] % GT_LINES, i, w = v->ph[0], period, dl, N, fr;
    int16_t *L = gt_line[li];
    int32_t lp = v->s[1], d = v->s[2], fb = v->s[3], dcx = v->s[5], dcy = v->s[6], tl = (int32_t)v->ph[2];
    int32_t tone = 3000 + (t->p[P_E7] * 29767) / 127, peak = 0;

    if (gt_owner[li] != v) {                            /* the line went to a newer note */
        v->active = 0;
        return;
    }
    period = gt_period_q16(m->inc);
    if (t->p[P_E6])                                     /* DRIFT: up to +-6.7 cents */
        period += (uint32_t)(((int64_t)period * v->s[4] * t->p[P_E6]) >> (15 + 7 + 8));
    if (!v->gate && !(v->s[7] & 1)) {                   /* let go: the finger damps the string */
        int32_t ms = 250 - (t->p[P_E4] * 225) / 127;    /* CHOP: 250 .. 25 ms */
        int32_t rel = gt_loss(period, ms);
        v->s[7] |= 1;
        if (rel < fb)
            v->s[3] = fb = rel;
        v->s[2] = d = (d * 3) >> 2;
        v->ph[1] = gt_filter_delay(d);
    }
    dl = v->ph[1];
    period = period > dl + (2u << 16) ? period - dl : 2u << 16;
    N = period >> 16;
    fr = (period & 0xFFFFu) >> 1;                       /* Q15 */

    for (i = 0; i < n; i++) {
        uint32_t r = (w - N) & GT_MASK;
        int32_t a = L[r], b = L[(r - 1u) & GT_MASK], x, y, o;
        x = a + (((b - a) * (int32_t)fr) >> 15);
        lp += ((x - lp) * d) >> 15;
        y = (lp * fb + 16384) >> 15;
        L[w & GT_MASK] = gt_sat16(y);
        w++;
        o = lp - dcx + ((dcy * 32604) >> 15);           /* DC blocker, ~35 Hz */
        dcx = lp;
        dcy = o;
        tl += ((o - tl) * tone) >> 15;                  /* TONE */
        out[i] += mulq15(mulq15(tl, amp_at(m, i)), VOICE_FS) << 1;
        o = tl < 0 ? -tl : tl;
        if (o > peak)
            peak = o;
    }
    v->ph[0] = w & GT_MASK;
    v->s[1] = lp;
    v->s[5] = dcx;
    v->s[6] = dcy;
    v->ph[2] = (uint32_t)tl;
    if (peak < 4) {                                     /* rung out: ~0.36 s of silence ends the voice */
        v->s[7] += 1 << 8;
        if ((v->s[7] >> 8) > 500)
            v->active = 0;
    } else
        v->s[7] &= 0xFF;
}

/* the factory sounds: E0..E7 = PICK BRIT SUST MUTE CHOP HARD DRIFT TONE; ADSR held at SUS 127 so the string's own
 * decay is heard; the amps as amp.c numbers them (1 CLEAN, 2 CRNCH, 3 HIGH, 4 LEAD, 5 BASS, 6 JANGL, 7 SKANK); the
 * rhythm sounds an octave under the keys (TRN -12), as M-PYRE's guitars */
static const preset_t GT_PRESETS[] = {
    /* INDIE: chiming cleans, chorus and a dotted-eighth delay, let ring */
    {"JANGLE", {30, 110, 88, 0, 20, 90, 40, 127}, {0, 127, 127, 40}, 0, 0, FX(0, 40, 30, 26),
     .x = {P_AMP + 1, 6, P_AGAIN + 1, 70, P_ATREB + 1, 76, P_STRUM + 1, 14, P_TRANS + 1, -12}},
    {"INDIE CRN", {24, 92, 92, 0, 30, 96, 30, 110}, {0, 127, 127, 30}, 0, 0, FX(0, 12, 20, 20),
     .x = {P_AMP + 1, 2, P_AGAIN + 1, 66, P_AMID + 1, 72, P_STRUM + 1, 10, P_TRANS + 1, -12}},
    {"DREAM ARP", {40, 100, 100, 0, 10, 70, 60, 100}, {0, 127, 127, 70}, 0, 0, FX(0, 60, 54, 50),
     .x = {P_AMP + 1, 6, P_AGAIN + 1, 60, P_TRANS + 1, -12}},
    /* METAL: tight, bright pick, palm mutes from the LIVE view or a step's ART */
    {"SYN CHUG", {10, 96, 104, 0, 70, 120, 10, 127}, {0, 127, 127, 12}, 0, 0, FX(0, 0, 0, 0),
     .x = {P_AMP + 1, 3, P_AGAIN + 1, 92, P_AMID + 1, 46, P_ATREB + 1, 74, P_STRUM + 1, 4, P_TRANS + 1, -12}},
    {"SYN DJENT", {6, 100, 96, 30, 90, 127, 6, 127}, {0, 127, 127, 8}, 0, 0, FX(0, 0, 0, 0),
     .x = {P_AMP + 1, 3, P_AGAIN + 1, 110, P_AMID + 1, 40, P_APRES + 1, 84, P_STRUM + 1, 3, P_TRANS + 1, -12}},
    {"SYN LEAD", {18, 104, 116, 0, 40, 100, 0, 120}, {0, 127, 127, 50}, 0, 1, FX(0, 0, 34, 26),
     .x = {P_AMP + 1, 4, P_AGAIN + 1, 86, P_AMID + 1, 74, P_LEG + 1, 30, P_VIB + 1, 30}},
    /* REGGAE: thin, bright chops choked on the key-up; the skank on the off-beats (SEQ > PATTERNS) */
    {"ONE DROP", {36, 96, 60, 20, 120, 100, 20, 120}, {0, 127, 127, 4}, 0, 0, FX(0, 0, 18, 30),
     .x = {P_AMP + 1, 7, P_AGAIN + 1, 60, P_ABASS + 1, 30, P_ATREB + 1, 84, P_STRUM + 1, 8}},
    {"DUB CHOP", {40, 80, 70, 30, 110, 90, 30, 96}, {0, 127, 127, 6}, 0, 0, FX(0, 0, 70, 46),
     .x = {P_AMP + 1, 7, P_AGAIN + 1, 56, P_ABASS + 1, 26, P_AMID + 1, 80, P_STRUM + 1, 10}},
    {"SKA UP", {32, 104, 56, 10, 127, 110, 10, 127}, {0, 127, 127, 3}, 0, 0, FX(0, 8, 0, 24),
     .x = {P_AMP + 1, 7, P_AGAIN + 1, 64, P_ATREB + 1, 90, P_STRUM + 1, -7}},
    {"STRING DI", {24, 90, 90, 0, 30, 90, 20, 127}, {0, 127, 127, 30}, 0, 0, FX(0, 0, 0, 0), .x = {P_TRANS + 1, -12}},
};

static const engine_t ENG_GUITAR = {
    "GUITAR", {"STRING", "HAND"},
    {
        {"PICK", F_PCT, 0, 127, 30, 0, 0},
        {"BRIT", F_PCT, 0, 127, 100, 0, 0},
        {"SUST", F_PCT, 0, 127, 90, 0, 0},
        {"MUTE", F_PCT, 0, 127, 0, 0, 0},
        {"CHOP", F_PCT, 0, 127, 30, 0, 0},
        {"HARD", F_PCT, 0, 127, 90, 0, 0},
        {"DRIFT", F_PCT, 0, 127, 30, 0, 0},
        {"TONE", F_PCT, 0, 127, 127, 0, 0},
    },
    GT_PRESETS, sizeof(GT_PRESETS) / sizeof(GT_PRESETS[0]), -1, gt_note_on, gt_render,
    0xFD20, {P_E1, P_E2, P_E3, P_E4},
    6,                                                  /* six strings */
};
