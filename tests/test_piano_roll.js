/* The Snoop level's piano roll page (canvas.js), driven the way the Schwung UI drives a canvas page:
 * drawPage(ctx, { values, width, height, nowMs }) with a frame-scoped context. Run: node tests/test_piano_roll.js */
"use strict";
const path = require("path");
const { overlay, parse, windowStart, noteName, noteHz } = require(path.join(__dirname, "..", "canvas.js"));

let checks = 0, failed = 0;
function check(cond, what) {
    checks++;
    if (!cond) { failed++; console.log("  FAIL: " + what); }
}

/* A band of the given size. Like the host's frame context it offers only fillRect, setPixel, line, print, textWidth,
 * and it complains about any drawing outside the band. */
function makeCtx(W, H) {
    const px = new Uint8Array(W * H), texts = [];
    const inb = (x, y, w, h, what) => check(w <= 0 || h <= 0 || (x >= 0 && y >= 0 && x + w <= W && y + h <= H), what + " outside the band: " + [x, y, w, h]);
    const ctx = {
        width: W, height: H, texts,
        fillRect(x, y, w, h, v) { inb(x, y, w, h, "fillRect"); for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) px[(y + j) * W + x + i] = v ? 1 : 0; },
        setPixel(x, y, v) { ctx.fillRect(x, y, 1, 1, v); },
        line() { },
        /* like the host's frame context: text that does not fit is truncated, text that starts outside is dropped */
        print(x, y, text) {
            check(x >= 0 && y >= 0 && y < H && x < W, "print starts inside the band: " + text + " at " + x + "," + y);
            let s = String(text);
            while (s.length && x + s.length * 6 > W) s = s.slice(0, -1);
            if (s) texts.push({ x, y, text: s });
        },
        textWidth(t) { return String(t).length * 6; },
        at(x, y) { return px[y * W + x]; },
        has(s) { return texts.some((t) => t.text.indexOf(s) >= 0); },
        dump() {
            const rows = [];
            for (let y = 0; y < H; y += 2) {
                let r = "";
                for (let x = 0; x < W; x += 2) r += (px[y * W + x] | px[y * W + x + 1] | (y + 1 < H ? px[(y + 1) * W + x] | px[(y + 1) * W + x + 1] : 0)) ? "#" : ".";
                rows.push(r);
            }
            return rows.join("\n");
        }
    };
    return ctx;
}

function page(W, H, values) {
    const ctx = makeCtx(W, H);
    overlay.drawPage(ctx, { values, width: W, height: H, nowMs: 0 });
    return ctx;
}

/* ---- helpers ---- */
check(noteName(60) === "C4" && noteName(69) === "A4" && noteName(50) === "D3" && noteName(61) === "C#4", "note names use scientific pitch");
check(noteHz(69).toFixed(1) === "440.0" && noteHz(50).toFixed(1) === "146.8" && noteHz(60).toFixed(1) === "261.6", "A4 = 440.0, D3 = 146.8, C4 = 261.6 Hz");
check(windowStart(50) === 36 && windowStart(60) === 48 && windowStart(12) === 0 && windowStart(5) === 0, "the window centres on the sounding octave");
check(windowStart(119) <= 96 && windowStart(127) === 96, "the window stays on the keyboard at the top");

const feed = "K,50,1,5,-3,80,52,57";                    /* D3, voice 2, D Min, 3 cents flat, 80%, E3 and A3 lately */
let p = parse({ snoop_feed: feed });
check(p.note === 50 && p.voice === 1 && p.key === 5 && p.cents === -3 && p.level === 80 && p.recent.join() === "52,57", "the feed is parsed");
p = parse({ snoop_note: "61", snoop_cents: "4", snoop_level: "9" });
check(p.note === 61 && p.cents === 4 && p.level === 9 && p.key === 24 && p.recent.length === 0, "plain values stand in until the feed arrives");
p = parse({ snoop_feed: "garbage", snoop_note: "x" });
check(p.note === 0 && p.key === 24, "an unreadable feed means idle and unknown key");
p = parse({ snoop_feed: "K,999,9,99,-99,500,5,xx,300" });
check(p.note === 127 && p.voice === 3 && p.key === 24 && p.recent.join() === "5", "out-of-range feed fields are clamped or dropped");
check(parse(undefined).note === 0 && parse(null).key === 24, "no values at all is fine");

/* ---- the roomy layout: a 128 x 40 band, D3 sounding ---- */
let ctx = page(128, 40, { snoop_feed: feed });
check(ctx.has("[D Min]"), "key banner");
check(ctx.texts.some((t) => t.text === "V2" && t.x + 12 === 127), "voice sits at the right edge");
check(ctx.has("D3 146.8Hz") && ctx.has("-3c"), "note, frequency and tuning error: " + JSON.stringify(ctx.texts.map((t) => t.text)));
const top = 19, kh = 40 - top - 1, ox = 1, ww = 9;      /* keyboard geometry for a 128 wide band */
const whiteX = (i) => ox + i * ww;
check(ctx.at(whiteX(8) + 4, top + 10) === 1, "D3 (white key 8 of the window starting at C2) is lit");
check(ctx.at(whiteX(0) + 4, top + 12) === 0 && ctx.at(whiteX(7) + 4, top + 12) === 0 && ctx.at(whiteX(9) + 4, top + 12) === 0, "other white keys are hollow");
check(ctx.at(whiteX(0), top + 5) === 1 && ctx.at(whiteX(0) + 4, top) === 1, "white keys have a border");
check(ctx.at(80 + 2, top + 3) === 1, "black keys are solid");
check(ctx.at(whiteX(9) + 4, top + kh - 4) === 1, "E3 (in the trail) is marked");
check(ctx.at(whiteX(12) + 4, top + kh - 4) === 1, "A3 (in the trail) is marked");
check(ctx.at(whiteX(10) + 4, top + kh - 4) === 0, "a key that did not sound is not marked");
console.log("\nD3 (146.8 Hz), voice 2, D Min, trail E3 and A3, in a 128 x 40 band:\n" + ctx.dump() + "\n");

/* ---- a black key ---- */
ctx = page(128, 40, { snoop_feed: "K,51,1,5,0,60" });
check(ctx.has("D#3"), "black key named");
check(ctx.at(80, top + 4) === 1 && ctx.at(82, top + 4) === 0, "the sounding black key is inverted: bordered, hollow inside");
check(ctx.at(whiteX(8) + 4, top + 12) === 0 && ctx.at(whiteX(9) + 4, top + 12) === 0, "no white key lit at the same time");

/* ---- other notes ---- */
ctx = page(128, 40, { snoop_feed: "K,69,1,19,0,50" });
check(ctx.has("[A Min]") && ctx.has("A4 440.0Hz"), "A4 = 440.0 Hz, key A Min");
ctx = page(128, 40, { snoop_feed: "K,60,0,0,12,50" });
check(ctx.has("C4 261.6Hz") && ctx.has("+12c") && ctx.has("[C Maj]") && ctx.texts.some((t) => t.text === "V1"), "C4, sharp by 12 cents, C Maj, voice 1");
ctx = page(128, 40, { snoop_feed: "K,60,3,24,0,50" });
check(ctx.has("[--]") && ctx.texts.some((t) => t.text === "All"), "unknown key and all voices");

/* ---- idle: nothing lit, and the window stays near the last trail ---- */
ctx = page(128, 40, { snoop_feed: "K,0,1,5,0,0" });
check(ctx.has("idle"), "idle is shown");
let lit = 0;
for (let i = 0; i < 14; i++) for (let y = top + 2; y < top + 14; y++) lit += ctx.at(whiteX(i) + 4, y);
check(lit === 0, "no key is lit while idle");

/* ---- smaller bands ---- */
ctx = page(128, 28, { snoop_feed: feed });
check(ctx.has("D Min V2 D3 146.8Hz"), "a medium band gets one compact line: " + JSON.stringify(ctx.texts.map((t) => t.text)));
check(ctx.at(whiteX(8) + 4, 9 + 6) === 1, "and still lights D3");
ctx = page(128, 15, { snoop_feed: feed });
check(ctx.texts.length === 0, "a short band is all keyboard");
check(ctx.at(whiteX(8) + 4, 6) === 1, "D3 lit in a 15 px band");
ctx = page(128, 5, { snoop_feed: feed });
check(true, "a band too small for a keyboard draws nothing and does not throw");
ctx = page(40, 40, { snoop_feed: feed });
check(true, "a narrow band does not throw");

/* ---- the voice meters and the transpose ---- */
p = parse({ snoop_feed: feed, mix_feed: "M,100,50,0,4,2" });
check(p.levels.join() === "100,50,0" && p.mask === 4 && p.transpose === 2, "the mix feed is parsed");
p = parse({ snoop_feed: feed });
check(p.levels.join() === "0,0,0" && p.mask === 0 && p.transpose === 0, "and its absence is not an error");
for (const bad of ["M", "M,1,2", "X,1,2,3,4,5", 7, "M,a,b,c,d,e"]) parse({ mix_feed: bad });
ctx = page(128, 40, { snoop_feed: feed, mix_feed: "M,100,50,0,4,2" });
const mright = 128 - 1 - 12 - 4, mx = mright - require(path.join(__dirname, "..", "canvas.js")).METER_W;     /* "V2" is 12 px wide */
check(ctx.at(mx, 1) === 1 && ctx.at(mx + 27, 1) === 1 && ctx.at(mx + 27, 2) === 1, "voice 1 at 100% fills its whole meter");
check(ctx.at(mx + 13, 4) === 1 && ctx.at(mx + 20, 4) === 0, "voice 2 at 50% fills half of its meter");
check(ctx.at(mx, 7) === 1 && ctx.at(mx + 1, 7) === 1 && ctx.at(mx + 2, 7) === 0 && ctx.at(mx + 4, 7) === 1 && ctx.at(mx + 4, 8) === 0,
      "voice 3, muted, is a dotted line one pixel high");
check(ctx.has("[D Min +2]"), "the transpose is shown next to the key: " + JSON.stringify(ctx.texts.map((t) => t.text)));
ctx = page(128, 40, { snoop_feed: feed, mix_feed: "M,0,0,0,0,-3" });
check(ctx.has("[D Min -3]") && ctx.at(mx, 1) === 1 && ctx.at(mx + 1, 1) === 0, "a silent voice keeps a one pixel stub; a negative transpose is shown");
ctx = page(128, 40, { snoop_feed: feed });
check(ctx.has("[D Min]") && !ctx.has("+"), "no transpose, no mark");
ctx = page(60, 40, { snoop_feed: feed, mix_feed: "M,100,100,100,0,12" });
check(true, "a narrow band draws no meters and does not throw");

/* ---- bad input never throws ---- */
for (const v of [undefined, null, {}, { snoop_feed: 5 }, { snoop_feed: "K" }, { snoop_note: {} }]) page(128, 40, v);
overlay.drawPage(makeCtx(128, 40), undefined);
check(true, "missing or odd payloads do not throw");

/* ---- every note a SID can sensibly play lies inside its window; above it nothing is lit ---- */
let outside = 0;
for (let n = 1; n <= 119; n++) { const rel = n - windowStart(n); if (rel < 0 || rel >= 24) outside++; }
check(outside === 0, "notes 1..119 fall inside their window");
ctx = page(128, 40, { snoop_feed: "K,125,1,5,0,50" });
let litTop = 0;
for (let i = 0; i < 14; i++) for (let y = top + 2; y < top + 14; y++) litTop += ctx.at(whiteX(i) + 4, y);
check(ctx.has("F9") && litTop === 0, "a note above the keyboard is named but lights nothing");

console.log("test_piano_roll: " + checks + " checks, " + failed + " failed");
process.exit(failed ? 1 : 0);
