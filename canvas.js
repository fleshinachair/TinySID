/* TinySID piano roll: a canvas PAGE on the Snoop level (module.json: "as_page": true, "page_first": true).
 *
 * The Schwung UI adds it to the level's page rotation, so it is on screen as soon as you open the Snoop level,
 * with no click, and the eight encoders keep working on it exactly as on the level's knob grid. The host calls
 * drawPage() continuously with the module's cached parameter values; it may not read parameters itself. The page
 * is the body band between the host's header and footer, so (0, 0) is the band's top-left and nothing drawn here
 * can leave it. Only these drawing calls exist: fillRect, setPixel, line, print, textWidth.
 *
 * Everything shown comes from one packed string the DSP formats (src/dsp/tinysid.c, snoop_feed), because the host
 * refreshes a page's extra keys only a few times a second:
 *     K,<note>,<voice 0..3>,<key 0..24>,<cents>,<level %>,<recent notes, newest first>
 * where key = tonic * 2 + (minor ? 1 : 0), 24 = unknown, note 0 = idle, voice 3 = all voices (the key is shown as the song
 * is transposed). A second string, mix_feed, carries the three voices' levels and which are muted from the pads:
 *     M,<level 1 %>,<level 2 %>,<level 3 %>,<mute mask>,<transpose>
 * They are drawn as three small meters in the banner (a muted voice is a dotted line), with the transpose next to the key.
 *
 * Layout (adapts to the band's height): key banner and voice, the note with its frequency and tuning error, and a
 * two-octave keyboard centred on the sounding octave. White keys are hollow, black keys solid, the sounding key is
 * inverted, and notes that sounded in the last half second leave a mark on their key.
 */
(function () {
    "use strict";

    var NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
    var KEY_NAMES = (function () {
        var out = [];
        for (var i = 0; i < 12; i++) { out.push(NAMES[i] + " Maj"); out.push(NAMES[i] + " Min"); }
        out.push("--");
        return out;
    })();
    var VOICES = ["V1", "V2", "V3", "All"];
    var WHITE_PC = [0, 2, 4, 5, 7, 9, 11];            /* pitch classes of the white keys within an octave */
    var BLACK_AFTER = [0, 1, 3, 4, 5];                /* black keys follow white keys C, D, F, G, A */
    var BLACK_PC = [1, 3, 6, 8, 10];
    var OCTAVES = 2, WINDOW = 12 * OCTAVES, WHITES = 7 * OCTAVES;

    function noteName(n) { return NAMES[n % 12] + (Math.floor(n / 12) - 1); }
    function noteHz(n) { return 440 * Math.pow(2, (n - 69) / 12); }

    /* First note of the two-octave window: the C that puts the sounding note closest to the middle. */
    function windowStart(note) {
        var start = 12 * Math.round((note - 12) / 12);
        if (start < 0) start = 0;
        if (start > 128 - WINDOW) start = 12 * Math.floor((128 - WINDOW) / 12);
        return start;
    }

    function toInt(v, dflt) {
        var n = parseInt(v, 10);
        return isNaN(n) ? dflt : n;
    }

    /* The page's data. The feed wins; the plain parameter values are the fallback when it has not arrived yet. */
    function parse(values) {
        var v = values || {};
        var s = { note: 0, voice: 1, key: 24, cents: 0, level: 0, recent: [], levels: [0, 0, 0], mask: 0, transpose: 0 };
        s.note = Math.max(0, Math.min(127, toInt(v.snoop_note, 0)));
        s.cents = toInt(v.snoop_cents, 0);
        s.level = toInt(v.snoop_level, 0);
        if (typeof v.snoop_feed === "string" && v.snoop_feed.charAt(0) === "K") {
            var f = v.snoop_feed.split(",");
            if (f.length >= 6) {
                s.note = Math.max(0, Math.min(127, toInt(f[1], 0)));
                s.voice = Math.max(0, Math.min(3, toInt(f[2], 1)));
                s.key = Math.max(0, Math.min(24, toInt(f[3], 24)));
                s.cents = toInt(f[4], 0);
                s.level = toInt(f[5], 0);
                for (var i = 6; i < f.length; i++) {
                    var n = toInt(f[i], -1);
                    if (n >= 0 && n < 128) s.recent.push(n);
                }
            }
        }
        if (typeof v.mix_feed === "string" && v.mix_feed.charAt(0) === "M") {
            var m = v.mix_feed.split(",");
            if (m.length >= 6) {
                for (var j = 0; j < 3; j++) s.levels[j] = Math.max(0, Math.min(100, toInt(m[1 + j], 0)));
                s.mask = toInt(m[4], 0);
                s.transpose = Math.max(-12, Math.min(12, toInt(m[5], 0)));
            }
        }
        return s;
    }

    var METER_W = 28;

    /* Three one-pixel-high-ish bars (2 px, 1 px apart): a voice's level, or a dotted line when it is muted. Returns the left edge. */
    function drawMeters(ctx, right, y, s) {
        var x = right - METER_W;
        for (var i = 0; i < 3; i++) {
            var yy = y + i * 3;
            if ((s.mask >> i) & 1) {
                for (var k = 0; k < METER_W; k += 4) ctx.fillRect(x + k, yy, 2, 1, 1);
            } else {
                ctx.fillRect(x, yy, Math.max(1, Math.round(s.levels[i] * METER_W / 100)), 2, 1);
            }
        }
        return x;
    }

    function tw(ctx, text) {
        return typeof ctx.textWidth === "function" ? ctx.textWidth(text) : text.length * 6;
    }

    /* A rectangular outline from filled rectangles (the page context has no drawRect). */
    function frame(ctx, x, y, w, h, c) {
        ctx.fillRect(x, y, w, 1, c);
        ctx.fillRect(x, y + h - 1, w, 1, c);
        ctx.fillRect(x, y, 1, h, c);
        ctx.fillRect(x + w - 1, y, 1, h, c);
    }

    /* The keyboard fills (x, y, w, h). Returns its geometry for the tests. */
    function drawKeys(ctx, x, y, w, h, s) {
        var ww = Math.floor(w / WHITES);
        if (ww < 3 || h < 6) return null;
        var kw = ww * WHITES, ox = x + Math.floor((w - kw) / 2);
        var bw = Math.max(3, ww - 4), bh = Math.max(3, Math.round(h * 0.62));
        var start = windowStart(s.note > 0 ? s.note : (s.recent.length ? s.recent[0] : 60));
        var sounding = s.note;

        /* white keys: an outer frame, a divider after each key, and the sounding one filled */
        frame(ctx, ox, y, kw + 1, h, 1);
        var i, o, b;
        for (i = 0; i < WHITES; i++) {
            var note = start + 12 * Math.floor(i / 7) + WHITE_PC[i % 7], kx = ox + i * ww;
            if (i > 0) ctx.fillRect(kx, y, 1, h, 1);
            if (note === sounding) ctx.fillRect(kx + 1, y + 1, ww - 1, h - 2, 1);
        }
        /* black keys, each with a clear gap round it so it separates from a filled white key behind it */
        for (o = 0; o < OCTAVES; o++) {
            for (b = 0; b < 5; b++) {
                var bx = ox + (o * 7 + BLACK_AFTER[b] + 1) * ww - Math.floor(bw / 2);
                var bn = start + 12 * o + BLACK_PC[b];
                ctx.fillRect(bx - 1, y + 1, bw + 2, bh, 0);
                ctx.fillRect(bx, y + 1, bw, bh - 1, 1);
                if (bn === sounding) ctx.fillRect(bx + 1, y + 2, bw - 2, bh - 3, 0);    /* inverted: hollow with a border */
            }
        }
        /* a mark on every key that sounded lately */
        for (i = 0; i < s.recent.length; i++) {
            var rn = s.recent[i] - start;
            if (rn < 0 || rn >= WINDOW) continue;
            var oct = Math.floor(rn / 12), pc = rn % 12;
            var wi = WHITE_PC.indexOf(pc);
            if (wi >= 0) {
                ctx.fillRect(ox + (oct * 7 + wi) * ww + 2, y + h - 4, Math.max(1, ww - 3), 2, 1);
            } else {
                var bi = BLACK_PC.indexOf(pc);
                var mx = ox + (oct * 7 + BLACK_AFTER[bi] + 1) * ww - Math.floor(bw / 2);
                ctx.fillRect(mx + 1, y + bh - 3, Math.max(1, bw - 2), 2, 0);
            }
        }
        return { ox: ox, ww: ww, kw: kw, bw: bw, bh: bh, start: start };
    }

    function drawPage(ctx, payload) {
        var p = payload || {};
        var s = parse(p.values);
        var W = ctx.width, H = ctx.height;
        var key = KEY_NAMES[s.key] || "--", voice = VOICES[s.voice] || "";
        var tuned = s.note > 0 ? noteName(s.note) + " " + noteHz(s.note).toFixed(1) + "Hz" : "idle";
        var cents = s.note > 0 && s.cents !== 0 ? " " + (s.cents > 0 ? "+" : "") + s.cents + "c" : "";
        var top;

        if (H >= 34) {                                    /* banner, note line, keyboard */
            var trans = s.transpose ? " " + (s.transpose > 0 ? "+" : "") + s.transpose : "";
            ctx.print(1, 0, "[" + key + trans + "]", 1);
            if (voice) ctx.print(W - 1 - tw(ctx, voice), 0, voice, 1);
            var meterRight = W - 1 - tw(ctx, voice || "All") - 4;
            if (meterRight - METER_W > tw(ctx, "[" + key + trans + "]") + 4) drawMeters(ctx, meterRight, 1, s);
            ctx.print(1, 9, tuned + cents, 1);
            top = 19;
        } else if (H >= 20) {                             /* one compact line */
            var line = key + " " + voice + " " + (s.note > 0 ? noteName(s.note) + " " + noteHz(s.note).toFixed(1) + "Hz" : "idle");
            ctx.print(1, 0, line, 1);
            top = 9;
        } else {
            top = 0;
        }
        drawKeys(ctx, 1, top, W - 2, H - top - 1, s);
    }

    globalThis.canvas_overlay = { drawPage: drawPage };

    /* test hook: lets a Node script drive the same code */
    if (typeof module !== "undefined" && module.exports) {
        module.exports = { overlay: globalThis.canvas_overlay, parse: parse, windowStart: windowStart, noteName: noteName,
                           noteHz: noteHz, drawKeys: drawKeys, KEY_NAMES: KEY_NAMES, METER_W: METER_W };
    }
})();
