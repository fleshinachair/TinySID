/* Runs the generated ui_chain.js (the module's own editor: pages, pad controls and lights, the SID picker) against the Schwung
 * UI's OWN libraries (page controller, input decoder, LED helpers, file browser state) with a fake DSP, display and filesystem.
 * The host code is not part of this repo: tools/fetch_host_ref.sh copies it from a Move into host_ref/ (git-ignored), or point
 * SCHWUNG_SHARED at a copy of the Move's /data/UserData/schwung/shared directory. scripts/test.sh generates the file into
 * build/gen.
 *
 *   node tests/test_ui_chain.mjs        (skips, exit 0, when no host reference is available) */
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const root = path.join(path.dirname(fileURLToPath(import.meta.url)), "..");
const shared = process.env.SCHWUNG_SHARED || path.join(root, "host_ref", "shared");
const gen = path.join(root, "build", "gen", "ui_chain.js");
const chainParamsFile = path.join(root, "build", "gen", "json", "chain_params.json");
if (!fs.existsSync(path.join(shared, "param_pages", "page_controller.mjs"))) {
    console.log("test_ui_chain: no host reference (run tools/fetch_host_ref.sh); skipped");
    process.exit(0);
}
if (!fs.existsSync(gen) || !fs.existsSync(chainParamsFile)) {
    console.log("test_ui_chain: build/gen/ui_chain.js missing (scripts/test.sh generates it); skipped");
    process.exit(0);
}

let checks = 0, failed = 0;
const check = (cond, what) => { checks++; if (!cond) { failed++; console.log("  FAIL: " + what); } };

/* ---- the file under test, made importable off the device ---- */
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "tinysid-ui-"));
fs.writeFileSync(path.join(tmp, "os_stub.mjs"), "export const readdir = (p) => globalThis.__fs.readdir(p);\nexport const stat = (p) => globalThis.__fs.stat(p);\n");
let src = fs.readFileSync(gen, "utf8");
check(src.includes("/data/UserData/schwung/shared/") && src.includes("from 'os'"), "the generated file imports the host's libraries from the device paths");
src = src.split("/data/UserData/schwung/shared/").join(pathToFileURL(shared).href + "/");
src = src.replace("from 'os'", "from '" + pathToFileURL(path.join(tmp, "os_stub.mjs")).href + "'");
check(!src.includes("@HIERARCHY@") && !src.includes("/*@CANVAS@*/"), "the template's markers were all replaced");
check(src.includes("globalThis.canvas_overlay") && src.includes("function drawKeys"), "the piano roll is inlined");
const file = path.join(tmp, "ui_chain.mjs");
fs.writeFileSync(file, src);

/* ---- the fake device ---- */
let chainParams = fs.readFileSync(chainParamsFile, "utf8");
const params = new Map(Object.entries({
    pad_mode: "Play", voice_mutes: "0", transpose: "0", subtune: "1", song_bpm: "125", song_key: "D Min", v1_vol: "1", v2_vol: "1",
    v3_vol: "1", out_trim: "0", sync_mode: "Free (PAL 50Hz)", tempo_trim: "1",
    sid_file: "/data/UserData/schwung/modules/sound_generators/tinysid/sids/a.sid",
    snoop_feed: "K,50,1,5,-3,80,52,57", mix_feed: "M,100,50,0,4,2", fx_count: "3", midi_out_v1: "Off", midi_out_v2: "Off", midi_out_v3: "Off",
}));
let sets = [], padBlocks = [], leds = [], printed = [], filled = 0, nulls = false;
globalThis.host_module_get_param = (k) => (nulls ? null : k === "chain_params" ? chainParams : params.has(k) ? params.get(k) : null);
/* Enough of the engine's pad_press for the screen: it carries out a press by writing the state the screen polls, and acknowledges it. */
let padAck = 0;
function fakePadPress(v) {
    const f = String(v).split(","), note = parseInt(f[0], 10), vel = parseInt(f[1], 10), i = note - 68;
    const put = (key, val) => { sets.push([key, String(val)]); params.set(key, String(val)); };
    const mutesNow = parseInt(params.get("voice_mutes") || "0", 10), tr = parseInt(params.get("transpose") || "0", 10);
    if (i >= 0 && i < 3) put("voice_mutes", mutesNow ^ (1 << i));
    else if (i === 3) put("voice_mutes", 0);
    else if (i >= 4 && i <= 6) put("transpose", i === 6 ? 0 : Math.max(-12, Math.min(12, tr + (i === 5 ? 1 : -1))));
    else if (i >= 8 && i <= 10) { const key = "midi_out_v" + (i - 7); put(key, params.get(key) === "On" ? "Off" : "On"); }
    else if (i >= 24 && i - 24 < parseInt(params.get("fx_count") || "0", 10)) put("fx_trigger", (i - 24) + "," + vel);
    else return;
    padAck++;
    params.set("pad_ack", padAck + "," + note + ",0");
}
globalThis.host_module_set_param = (k, v) => { sets.push([k, String(v)]); params.set(k, String(v)); if (k === "pad_press") fakePadPress(v); };
globalThis.host_pad_block = (v) => { padBlocks.push(v); };
globalThis.move_midi_internal_send = (pkt) => { leds.push(Array.from(pkt)); return 1; };
globalThis.clear_screen = () => { printed = []; filled = 0; };
globalThis.fill_rect = () => { filled++; };
globalThis.set_pixel = () => { filled++; };
globalThis.print = (x, y, t) => { printed.push(String(t)); };
globalThis.text_width = (t) => String(t).length * 6;
const dirs = "/data/UserData/schwung/modules/sound_generators/tinysid/sids";
const tree = { [dirs]: ["a.sid", "b.sid", "notes.txt", "sub"], [dirs + "/sub"]: ["c.sid"] };
globalThis.__fs = {
    readdir: (p) => [tree[p] || [], 0],
    stat: (p) => [{ mode: tree[p] ? 0o040000 : 0o100000 }, 0],
};

await import(pathToFileURL(file).href);
const ui = globalThis.chain_ui, hook = globalThis.chain_ui_test;
check(ui && ["init", "tick", "onMidiMessageInternal", "handleBack"].every((f) => typeof ui[f] === "function"), "chain_ui offers the four entry points the host calls");

const ticks = (n) => { for (let i = 0; i < n; i++) ui.tick(); };
const lastSet = (k) => { for (let i = sets.length - 1; i >= 0; i--) if (sets[i][0] === k) return sets[i][1]; return null; };
const ledOf = (note) => { for (let i = leds.length - 1; i >= 0; i--) if (leds[i][2] === note) return leds[i][3]; return null; };
const press = (note, vel = 100) => ui.onMidiMessageInternal([0x90, note, vel]);

/* ---- the pages ---- */
ui.init();
ticks(120);                                               /* the roll's feeds come round on the controller's slow read rotation */
const ctl = hook.controller();
const pages = ctl.pages || [];
check(pages.length >= 5, "the controller plans the module's pages: " + pages.length);
check(pages[0] && pages[0].canvas && pages[0].canvas.key === "piano_roll", "the first page is the piano roll");
check(ctl.pageIndex === 0, "and it opens on it");
check(printed.some((t) => t.includes("D Min")), "the roll is drawn, from the feeds: " + JSON.stringify(printed.slice(0, 6)));
check(printed.some((t) => t.includes("+2")), "with the transpose next to the key");
const tuneIdx = pages.findIndex((p) => p.keys && p.keys.includes("sid_file"));
check(tuneIdx > 0, "a Tune + Loop page holds the SID File cell (page " + tuneIdx + ")");
const padIdx = pages.findIndex((p) => p.keys && p.keys.includes("pad_mode"));
check(padIdx > 0, "the Pad + Filter page holds Pad Mode");

/* a knob turns a parameter */
sets = [];
ui.onMidiMessageInternal([0xB0, 71, 1]);
ticks(6);
check(sets.some((s) => s[0] === "v1_vol"), "turning knob 1 writes Voice 1's level: " + JSON.stringify(sets));

/* ---- Play mode: the pads stay the track's ---- */
padBlocks = []; leds = []; sets = [];
ticks(8);
check(padBlocks.length > 0 && padBlocks.every((v) => v === 0), "Play mode gives the pads back to the track (host_pad_block(0) every frame)");
check(leds.length === 0, "and paints nothing");
press(68);
check(lastSet("voice_mutes") === null, "a pad press in Play mode does nothing here");

/* ---- Control mode: the bottom row is the controls ---- */
params.set("pad_mode", "Control");
padBlocks = []; leds = [];
ticks(14);
check(padBlocks.slice(-6).length === 6 && padBlocks.slice(-6).every((v) => v === 1), "Control mode takes the pads (host_pad_block(1) every frame)");
check(leds.every((p) => p[0] === 0x09 && p[1] === 0x90 && p[2] >= 68 && p[2] <= 99 && p[3] >= 0 && p[3] <= 127), "every light is a note-on for a hardware pad");
const want = [33, 33, 33, 120, 3, 7, 127];
check(want.every((c, i) => ledOf(68 + i) === c), "the bottom row is lit: " + [0, 1, 2, 3, 4, 5, 6].map((i) => ledOf(68 + i)).join(","));
check(new Set(want).size === 5, "the three voice mutes share a colour, the other four controls have their own");

/* a press shows at once, before the engine has answered, and a slow engine does not flick it back */
{
    const realSet = globalThis.host_module_set_param;
    const queued = [];
    globalThis.host_module_set_param = (k, v) => { sets.push([k, String(v)]); if (k === "pad_press") queued.push(v); else params.set(k, String(v)); };
    leds = [];
    press(77, 100);
    check(ledOf(77) === 126, "the Notes Out pad goes green the moment it is pressed, not when the engine answers");
    ticks(8);
    check(ledOf(77) === 126, "and stays green while the engine has not caught up (the state reads are held off)");
    globalThis.host_module_set_param = realSet;
    for (const v of queued) fakePadPress(v);                                  /* the engine catches up */
    ticks(20);
    check(ledOf(77) === 126 && params.get("midi_out_v2") === "On", "then the engine's state agrees");
    press(77, 100);
    ticks(20);
    check(ledOf(77) === 119, "and a second press turns it off again");
}
check([79, 83, 90, 95, 99].every((n) => ledOf(n) === 0), "the rest of the grid is dark");
check(ledOf(75) === 66, "the eighth pad (loop capture) is very dark red while there is no loop");
check([76, 77, 78].every((n) => ledOf(n) === 119), "the row above the controls (Notes Out, voices 1 to 3) is grey while nothing is sent");
check([92, 93, 94].every((n) => ledOf(n) === 22), "the top row is lit purple for each sound effect the tune has (3): " + [92, 93, 94, 95].map(ledOf).join(","));

sets = [];
press(68); check(lastSet("voice_mutes") === "1", "pad 1 mutes voice 1");
press(69); check(lastSet("voice_mutes") === "3", "pad 2 mutes voice 2 as well");
ticks(2);
check(ledOf(68) === 119 && ledOf(69) === 119 && ledOf(70) === 33, "muted voices go grey, the other keeps its colour");
press(68); check(lastSet("voice_mutes") === "2", "pad 1 again unmutes voice 1");
press(70); check(lastSet("voice_mutes") === "6", "pad 3 mutes voice 3");
press(71); check(lastSet("voice_mutes") === "0", "pad 4 unmutes all");
press(72); check(lastSet("transpose") === "-1", "pad 5 transposes down");
press(72); check(lastSet("transpose") === "-2", "again");
press(73); check(lastSet("transpose") === "-1", "pad 6 transposes up");
press(74); check(lastSet("transpose") === "0", "pad 7 puts it back to 0");
for (let i = 0; i < 15; i++) press(73);
check(lastSet("transpose") === "12", "up stops at an octave");
const before = sets.length;
press(79); press(80); press(99); press(68, 0); ui.onMidiMessageInternal([0x80, 68, 0]);
check(sets.slice(before).every((s) => s[0] === "pad_press") && sets.slice(before).length === 3, "other pads change nothing (the engine is asked and does nothing), and releases are not sent at all");

/* the eighth pad captures a loop: flashing red while it records, steady red once it loops */
sets = [];
press(75, 100);
check(lastSet("pad_press") === "75,100", "the loop pad is handed to the engine");
const flashes = [];
for (let k = 0; k < 20; k++) { ticks(1); flashes.push(ledOf(75)); }
check(flashes.includes(1) && flashes.includes(0), "it flashes red while the loop is captured: " + flashes.join(","));
params.set("loop_capture", "recording");
press(75, 100);
ticks(2);
check(ledOf(75) === 1, "the second press: steady red, looping");
params.set("loop_capture", "looping");
ticks(30);
check(ledOf(75) === 1, "and it stays so with the engine's state");
params.set("loop_capture", "idle");
ticks(30);
check(ledOf(75) === 66, "when the loop is switched off, very dark red again");
press(74);

/* every pad press goes to the engine, which knows which notes are its own coming back */
sets = [];
ui.onMidiMessageInternal([0x90, 73, 100]);
check(lastSet("pad_press") === "73,100" && lastSet("transpose") === "1", "a press is handed to the engine as pad_press, which carries it out: " + lastSet("pad_press"));
ui.onMidiMessageInternal([0x90, 74, 64]);
check(lastSet("pad_press") === "74,64" && lastSet("transpose") === "0", "with its velocity");

/* the firmware's own writes to a pad are painted over the next frame, even when no note reaches this screen */
{
    const moveLeds = {};
    globalThis.shadow_get_pad_led_snapshot = () => moveLeds;
    ticks(3);
    leds = [];
    moveLeds[80] = 126; moveLeds[69] = 126;                 /* Move lights two pads green */
    ticks(1);
    check(ledOf(80) === 0 && ledOf(69) === 119 || ledOf(80) === 0 && ledOf(69) === 33, "a pad Move writes gets our colour back on the next frame: " + ledOf(80) + "," + ledOf(69));
    leds = [];
    ticks(4);
    check(leds.filter((p) => p[2] === 80 && p[3] === 0).length >= 3, "and keeps being repainted for a few frames, so ours lands after Move's");
    ticks(4);
    leds = [];
    ticks(3);
    check(leds.filter((p) => p[2] === 80).length <= 1, "then is left alone while Move leaves it alone");
    delete globalThis.shadow_get_pad_led_snapshot;
}

/* the firmware lights a pad when a note reaches it; any note event repaints the grid over the next frames */
leds = [];
ui.onMidiMessageInternal([0x91, 50, 100]);
ticks(4);
check(new Set(leds.map((p) => p[2])).size === 32, "a note event repaints all 32 pads within a few frames: " + new Set(leds.map((p) => p[2])).size);
ticks(4);

/* the row above the controls switches Notes Out on and off for each voice, apart from the audio mutes */
sets = [];
press(76); check(lastSet("midi_out_v1") === "On", "pad 1 of the second row sends voice 1 as MIDI");
ticks(2); check(ledOf(76) === 126 && ledOf(77) === 119, "and goes green, the others staying grey");
press(78); check(lastSet("midi_out_v3") === "On", "pad 3 sends voice 3");
press(77); check(lastSet("midi_out_v2") === "On", "pad 2 sends voice 2");
press(76); check(lastSet("midi_out_v1") === "Off", "pressing it again stops voice 1");
check(lastSet("voice_mutes") === null, "none of this touches the audio mutes");
ticks(2); check(ledOf(76) === 119 && ledOf(77) === 126 && ledOf(78) === 126, "the lights follow");
params.set("midi_out_v1", "On"); params.set("midi_out_v2", "Off"); params.set("midi_out_v3", "Off");
ticks(30);                                                  /* past the hold after the last press, and a poll */
check(ledOf(76) === 126 && ledOf(77) === 119 && ledOf(78) === 119, "a setting made on the Notes Out page lights them too");
press(68); check(lastSet("voice_mutes") === "1", "muting voice 1 in the audio ...");
check(params.get("midi_out_v1") === "On" && ledOf(76) === 126, "... leaves its MIDI as it was");
press(71);
params.set("midi_out_v1", "Off");
ticks(14);

/* the top row plays the tune's own sound effects */
sets = [];
press(92, 100);   check(lastSet("fx_trigger") === "0,100", "the first top pad plays effect 0 at the velocity it was hit with: " + lastSet("fx_trigger"));
check(ledOf(92) === 120, "and flashes white at once");
ticks(10);
check(ledOf(92) === 22, "then goes back to purple");
press(94, 1);     check(lastSet("fx_trigger") === "2,1", "the third plays effect 2, softly");
press(93, 127);   check(lastSet("fx_trigger") === "1,127", "the second plays effect 1, loud");
sets = [];
press(95, 100); press(99, 100); press(92, 0); ui.onMidiMessageInternal([0x80, 92, 0]);
check(lastSet("fx_trigger") === null, "pads with no effect behind them, and releases, do nothing");
params.set("fx_count", "0");
ticks(20);
check([92, 93, 94].every((n) => ledOf(n) === 0), "a tune with no effects leaves the top row dark");
press(92, 100);
check(lastSet("fx_trigger") === null, "and its pads do nothing");
params.set("fx_count", "3");
ticks(20);

/* leaving Control mode hands the pads back */
params.set("pad_mode", "Play");
padBlocks = []; leds = [];
ticks(14);                                                  /* the mode is polled every sixth frame: leave room for a late poll */
check(padBlocks.slice(-6).length === 6 && padBlocks.slice(-6).every((v) => v === 0), "back to Play: the pads are released");
check(leds.length >= 32 && leds.slice(-32).every((p) => p[3] === 0) && new Set(leds.slice(-32).map((p) => p[2])).size === 32, "and the whole grid is handed back as Move had it (dark, with no snapshot)");

/* the host takes the screen away (a slot change, another editor): it calls host_pad_block(0) on its own account every frame, and that
 * is the cue to hand the lights back to Move */
params.set("pad_mode", "Control");
ticks(14);
leds = [];
globalThis.host_pad_block(0);
check(leds.length >= 32 && new Set(leds.slice(-32).map((p) => p[2])).size === 32 && leds.slice(-32).every((p) => p[3] === 0), "when the host takes the pads back (a slot change) the lights go back to Move's: " + leds.length);
leds = [];
globalThis.host_pad_block(0);
check(leds.length === 0, "and only the once");
params.set("pad_mode", "Play");
ui.init();
ticks(8);
leds = [];
ticks(4); globalThis.host_pad_block(0);
check(leds.length === 0, "a host call while the lights are not up repaints nothing");

/* Move takes the display back (a track change, Menu): the pads and their lights go back to Move until our screen is up again */
params.set("pad_mode", "Control");
ticks(14);
let displayMode = 0;
globalThis.shadow_get_display_mode = () => displayMode;
padBlocks = []; leds = [];
ticks(3);
check(padBlocks.length > 0 && padBlocks.every((v) => v === 0), "with Move on screen the pads are released every frame");
check(leds.length >= 32 && leds.slice(-32).every((p) => p[3] === 0), "and the lights are handed back once");
leds = [];
ticks(5);
check(leds.length === 0, "and nothing is painted while Move has the display");
displayMode = 1;
padBlocks = []; leds = [];
ticks(8);
check(padBlocks.slice(-3).every((v) => v === 1), "back on screen: the pads are taken again");
check(ledOf(71) === 120 && ledOf(68) !== null, "and the control lights are painted afresh");
delete globalThis.shadow_get_display_mode;
params.set("pad_mode", "Play");
ticks(14);

/* Back: gives the pads back and lets the host leave */
params.set("pad_mode", "Control");
ticks(14);
padBlocks = [];
check(ui.handleBack() === false, "Back is not consumed when there is nothing to close");
check(padBlocks[padBlocks.length - 1] === 0, "and it releases the pads on the way out");
params.set("pad_mode", "Play");
ui.init();
ticks(8);

/* ---- the SID picker ---- */
/* the real gesture first: jog to the Tune page, hold the SID File knob, click */
{
    const c = hook.controller();
    const idx = c.pages.findIndex((p) => p.keys && p.keys.includes("sid_file"));
    const slot = c.pages[idx].keys.indexOf("sid_file");
    for (let i = 0; i < 20 && hook.controller().pageIndex < idx; i++) { ui.onMidiMessageInternal([0xB0, 14, 1]); ticks(2); }
    check(hook.controller().pageIndex === idx, "the jog reaches the Tune page: " + hook.controller().pageIndex + " of " + idx);
    ui.onMidiMessageInternal([0x90, slot, 127]);               /* hold the SID File knob */
    ticks(2);
    ui.onMidiMessageInternal([0xB0, 3, 127]);                 /* click */
    check(hook.picker() !== null, "holding the SID File knob and clicking opens the list (slot " + slot + ")");
    if (hook.picker()) ui.handleBack();
    ui.onMidiMessageInternal([0x80, slot, 0]);

    /* a new tune changes the contract (its subtune count, its instruments): the gesture must still work afterwards */
    const doc = JSON.parse(chainParams);
    for (const m of doc) { if (m.key === "subtune") m.max = 7; if (m.key === "pad_sound") m.options = ["A", "B", "C"]; }
    chainParams = JSON.stringify(doc);
    ticks(400);
    ui.onMidiMessageInternal([0x90, slot, 127]);
    ticks(2);
    ui.onMidiMessageInternal([0xB0, 3, 127]);
    check(hook.picker() !== null, "the list still opens after the contract changed (a new tune was loaded)");
    if (hook.picker()) ui.handleBack();
    ui.onMidiMessageInternal([0x80, slot, 0]);
}
const sidMeta = JSON.parse(chainParams).find((p) => p.key === "sid_file");
check(sidMeta && sidMeta.type === "filepath", "sid_file is a filepath parameter in the contract");
hook.openPicker("sid_file", sidMeta);
let pk = hook.picker();
check(pk && pk.state.items.map((i) => i.label).join("|") === "..|[sub]|a.sid|b.sid", "the picker lists the folder: " + (pk ? pk.state.items.map((i) => i.label).join("|") : "none"));
check(pk && pk.state.items[pk.state.selectedIndex].label === "a.sid", "with the current file selected");
ticks(2);
check(printed.some((t) => t.includes("a.sid")) && printed.some((t) => t.includes("b.sid")), "and draws it");
ui.onMidiMessageInternal([0xB0, 14, 1]);                  /* jog one detent clockwise */
check(hook.picker().state.items[hook.picker().state.selectedIndex].label === "b.sid", "the jog scrolls");
sets = [];
ui.onMidiMessageInternal([0xB0, 3, 127]);                 /* click */
check(lastSet("sid_file") === dirs + "/b.sid", "click picks the file: " + lastSet("sid_file"));
check(hook.picker() === null, "and closes the list");
hook.openPicker("sid_file", sidMeta);
ui.onMidiMessageInternal([0xB0, 14, 127]);                /* back up */
ui.onMidiMessageInternal([0xB0, 14, 127]);
ui.onMidiMessageInternal([0xB0, 3, 127]);                 /* click on [sub] */
check(hook.picker() && hook.picker().state.items.some((i) => i.label === "c.sid"), "a folder opens: " + (hook.picker() ? hook.picker().state.items.map((i) => i.label).join("|") : "none"));
sets = [];
ui.onMidiMessageInternal([0xB0, 51, 127]);                /* Back */
check(hook.picker() === null && lastSet("sid_file") === null, "Back closes the list and changes nothing");
hook.openPicker("sid_file", sidMeta);
check(ui.handleBack() === true && hook.picker() === null, "the host's Back is consumed while the list is up");

/* ---- a fault is shown on the screen, not swallowed ---- */
ui.onMidiMessageInternal(new Proxy([], { get() { throw new Error("boom"); } }));
ticks(1);
check(printed.some((t) => t.startsWith("midi: boom")), "an exception in a handler is drawn on the display: " + JSON.stringify(printed.slice(-3)));

/* ---- a cold start, with nothing readable yet, must not throw ---- */
nulls = true;
let threw = null;
try { ui.init(); ticks(10); ui.onMidiMessageInternal([0xB0, 71, 1]); press(68); ui.handleBack(); } catch (e) { threw = e; }
check(threw === null, "no reads answer yet: nothing throws" + (threw ? " (" + threw.message + ")" : ""));
nulls = false;
ui.init();
ticks(10);
check(hook.controller().pages.length >= 5, "and the pages arrive once the reads do");

/* ---- the debug log: off without the flag file; with it, the screen writes what it sees ---- */
let written = null;
globalThis.host_write_file = (p, body) => { written = { p, body }; return true; };
globalThis.host_file_exists = () => false;
ui.init(); ticks(70);
check(written === null, "no flag file, no log");
globalThis.host_file_exists = (p) => p.endsWith("tinysid_debug_on");
params.set("pad_mode", "Control");
ui.init(); ticks(14);
ui.onMidiMessageInternal([0x90, 70, 90]);
ticks(70);
check(written && written.p.endsWith("tinysid_debug.log"), "with the flag file the log is written");
const logText = written ? written.body : "";
check(["START", "MODE control", "IN 90 46 5a", "PRESS 70,90", "OUT 09 90", "STATE mode=1"].every((k) => logText.indexOf(k) >= 0),
      "and records input, presses, lights and state: " + logText.split(String.fromCharCode(10)).slice(0, 6).join(" | "));
globalThis.host_file_exists = () => false;
params.set("pad_mode", "Play");
ui.init();

fs.rmSync(tmp, { recursive: true, force: true });
console.log("test_ui_chain: " + checks + " checks, " + failed + " failed");
process.exit(failed ? 1 : 0);
