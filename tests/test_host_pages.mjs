/* Runs the Schwung UI's OWN modules (page planner, contract validator, widget frame) against this module's manifest
 * and canvas.js, so the Snoop level's piano roll is checked against the code that will show it, not only against
 * a mock. The host code is not part of this repo: tools/fetch_host_ref.sh copies it from a Move into host_ref/
 * (git-ignored), or point SCHWUNG_SHARED at a copy of the Move's /data/UserData/schwung/shared directory.
 * The documents are the ones the DSP serves: scripts/test.sh generates them into build/gen/json.
 *
 *   node tests/test_host_pages.mjs        (skips, exit 0, when no host reference is available) */
import fs from "node:fs";
import path from "node:path";
import vm from "node:vm";
import { fileURLToPath, pathToFileURL } from "node:url";

const root = path.join(path.dirname(fileURLToPath(import.meta.url)), "..");
const shared = process.env.SCHWUNG_SHARED || path.join(root, "host_ref", "shared");
const pp = (f) => path.join(shared, "param_pages", f);
if (!fs.existsSync(pp("page_plan.mjs"))) {
    console.log("test_host_pages: no host reference (run tools/fetch_host_ref.sh); skipped");
    process.exit(0);
}
const docs = path.join(root, "build", "gen", "json");
if (!fs.existsSync(path.join(docs, "chain_params.json"))) {
    console.log("test_host_pages: build/gen/json missing (scripts/test.sh generates it); skipped");
    process.exit(0);
}

const { planPages } = await import(pathToFileURL(pp("page_plan.mjs")).href);
const { validateContract } = await import(pathToFileURL(pp("validate_contract.mjs")).href);
const { frameCtx } = await import(pathToFileURL(pp("frame_ctx.mjs")).href);

const readJson = (n) => JSON.parse(fs.readFileSync(path.join(docs, n), "utf8"));
const hierarchy = readJson("ui_hierarchy.json"), chainParams = readJson("chain_params.json"), capabilities = readJson("capabilities.json");

let checks = 0, failed = 0;
const check = (cond, what) => { checks++; if (!cond) { failed++; console.log("  FAIL: " + what); } };

/* ---- the host's contract validator ---- */
const report = validateContract({ id: "tinysid", hierarchy, chainParams, capabilities });
const bad = report.findings.filter((f) => f.level === "error" || f.level === "warn");
check(bad.length === 0, "contract validator has no errors or warnings: " + bad.map((f) => f.rule + " (" + f.message.slice(0, 90) + ")").join("; "));
check(["snoop_feed", "mix_feed"].every((k) => chainParams.some((p) => p.key === k && p.type === "string" && p.access === "read")), "both feeds are declared as read-only strings");
check(capabilities.canvas_script === "canvas.js", "capabilities.canvas_script names the script");

/* ---- the host's page planner: what pages will appear, in what order ---- */
const plan = planPages({ hierarchy, chainParams });
const pages = plan.pages;
const rootPages = pages.filter((p) => p.level === "root");
const mixerKeys = ["v1_vol", "v2_vol", "v3_vol", "sync_mode", "tempo_trim", "song_bpm", "song_key"];
check(rootPages.length === 2, "the Mixer level has two pages: the piano roll and its knob grid (found " + rootPages.length + ")");
check(rootPages[0] && rootPages[0].canvas && rootPages[0].canvas.key === "piano_roll", "the piano roll page comes FIRST (page_first), so opening the module shows it");
check(rootPages[0] && rootPages[0].canvas.script === "canvas.js" && JSON.stringify(rootPages[0].canvas.extraKeys) === '["snoop_feed","mix_feed"]', "it runs canvas.js and reads the snoop_feed and mix_feed keys");
check(rootPages.every((p) => JSON.stringify(p.keys) === JSON.stringify(mixerKeys)), "both pages carry the mixer encoders, so the voice levels move while you watch the meters: " + JSON.stringify(rootPages.map((p) => p.keys)));
check(rootPages[1] && !rootPages[1].canvas, "the second Mixer page is the ordinary knob grid");
check(!pages.some((p) => p.level === "song"), "there is no Song level any more");
const padPage = pages.filter((p) => p.level === "pad");
check(padPage.length === 1 && JSON.stringify(padPage[0].keys) === JSON.stringify(["filter_mode", "cutoff_bias", "res_bias", "chip_model", "pad_target", "pad_sound", "pad_mode"]), "filter, chip, the pad and its mode share one page, the filter trio inside one row (the host leaves the author's order alone)");
check(!pages.some((p) => p.keys.includes("piano_roll") || p.keys.includes("snoop_feed")), "neither the canvas nor the feed becomes a knob cell");
const gluePage = pages.filter((p) => p.level === "glue");
check(gluePage.length === 1 && JSON.stringify(gluePage[0].keys) === JSON.stringify(["stereo_spread", "ducking_depth", "ducking_release", "fx_duck", "filter_pump", "lofi", "transpose"]), "Mix + Glue is one page, with FX Duck and Transpose: " + JSON.stringify(gluePage.map((p) => p.keys)));
const tunePage = pages.filter((p) => p.level === "tune");
check(tunePage.length === 1 && JSON.stringify(tunePage[0].keys) === JSON.stringify(["loop_length", "loop_start", "loop_nudge", "sid_file", "subtune", "timbre_lock", "pitch_snap", "master_tune"]), "Tune + Loop is one page, with Timbre Lock, AutoTune and Master Tune after Subtune: " + JSON.stringify(tunePage.map((p) => p.keys)));
check(pages.map((p) => p.level).join() === "root,root,tune,glue,pad,midi", "page order: " + pages.map((p) => p.level).join());
const midiPage = pages.filter((p) => p.level === "midi");
check(midiPage.length === 1 && JSON.stringify(midiPage[0].keys) === JSON.stringify(["midi_out_v1", "midi_out_ch1", "midi_out_v2", "midi_out_ch2", "midi_out_v3", "midi_out_ch3", "audio_delay"]), "Notes Out is its own page: a send and a channel for each voice, and the SID delay: " + JSON.stringify(midiPage.map((p) => p.keys)));
check((plan.warnings || []).length === 0, "the planner raised no warnings: " + JSON.stringify(plan.warnings));

/* ---- the page script, loaded the way the host loads it, drawn through the host's real frame context ---- */
const sandbox = {};
vm.createContext(sandbox);
vm.runInContext(fs.readFileSync(path.join(root, "canvas.js"), "utf8"), sandbox, { filename: "canvas.js" });
const overlay = sandbox.canvas_overlay;
check(overlay && typeof overlay.drawPage === "function", "canvas.js sets canvas_overlay.drawPage");

function hostScreen() {                                       /* the 128 x 64 display the frames draw onto */
    const px = new Uint8Array(128 * 64), texts = [];
    return {
        px, texts,
        fillRect(x, y, w, h, c) { for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) px[(y + j) * 128 + x + i] = c ? 1 : 0; },
        print(x, y, t) { texts.push({ x, y, text: t }); },
        textWidth(t) { return String(t).length * 6; },
        at(x, y) { return px[y * 128 + x]; }
    };
}

const bands = [{ y: 12, h: 40 }, { y: 12, h: 44 }, { y: 10, h: 34 }, { y: 12, h: 28 }, { y: 12, h: 20 }];
for (const b of bands) {
    const screen = hostScreen();
    const f = frameCtx(screen, { x: 0, y: b.y, w: 128, h: b.h });
    overlay.drawPage(f, {
        key: "piano_roll", keys: mixerKeys, touched: -1, nowMs: 0, width: 128, height: b.h, state: {},
        values: { snoop_voice: "Voice 2 (Lead)", snoop_note: "50", snoop_level: "80", snoop_feed: "K,50,1,5,-3,80,52,57" }
    });
    check(f.clipped() === 0, `band ${b.h} px high: nothing is clipped, truncated or dropped (${f.clipped()})`);
    const drewKeys = screen.px.some((v) => v === 1);
    check(drewKeys, `band ${b.h} px high: the keyboard is drawn`);
    check(screen.texts.length > 0 && screen.texts.every((t) => t.y >= b.y), `band ${b.h} px high: text stays below the host header`);
    /* nothing outside the band: the rows above it stay empty */
    let stray = 0;
    for (let y = 0; y < b.y; y++) for (let x = 0; x < 128; x++) stray += screen.at(x, y);
    check(stray === 0, `band ${b.h} px high: nothing is drawn above the band`);
    if (b.h === 40) {
        const top = b.y + 19, whiteX = (i) => 1 + i * 9;
        check(screen.at(whiteX(8) + 4, top + 10) === 1, "D3's key is lit through the host frame");
        check(screen.at(whiteX(0) + 4, top + 12) === 0, "an unplayed key is hollow through the host frame");
    }
}

/* the page keeps working when the feed has not arrived or the module is idle */
for (const values of [{}, { snoop_feed: "" }, { snoop_note: "0" }, { snoop_feed: "K,0,3,24,0,0" }]) {
    const screen = hostScreen();
    const f = frameCtx(screen, { x: 0, y: 12, w: 128, h: 40 });
    overlay.drawPage(f, { values, width: 128, height: 40, nowMs: 0, state: {} });
    check(f.clipped() === 0, "idle or missing values draw cleanly: " + JSON.stringify(values));
}

/* the host retires a page script that throws: ours must not, whatever it is handed */
let threw = false;
try {
    for (const v of [undefined, null, 5, "x", { snoop_feed: { a: 1 } }]) {
        overlay.drawPage(frameCtx(hostScreen(), { x: 0, y: 12, w: 128, h: 40 }), { values: v, width: 128, height: 40 });
    }
    overlay.drawPage(frameCtx(hostScreen(), { x: 0, y: 12, w: 128, h: 40 }), undefined);
} catch (e) { threw = true; console.log("  threw: " + e); }
check(!threw, "odd payloads never throw (a throw would retire the page for the session)");

console.log(`test_host_pages: ${checks} checks, ${failed} failed`);
process.exit(failed ? 1 : 0);
