/*
 * TinySID's editor (ui_chain.js). GENERATED at build time by tools/gen_ui_chain.py from this template, canvas.js (the piano
 * roll, inlined below) and the module's own hierarchy (module.json), so the manifest stays the one source of truth.
 *
 * WHY THIS FILE EXISTS. The host's stock editor lights no pads and cannot put a control on a fixed pad: a pad press reaches a
 * module as a note whose number depends on the Move's scale layout. A component that ships its own ui_chain.js owns the pads
 * (host_pad_block) and sees them as hardware pads 68..99, bottom-left to top-right, which is what lets TinySID's Pad Mode
 * Control put its buttons on the bottom row and colour them. The host loads this file only for a component whose DSP serves NO
 * ui_hierarchy, so the DSP refuses that key while this file sits beside it (plugin.c) and the hierarchy is supplied from here
 * to a controller this file builds, the way trance-gate (graebe, MIT) does. Delete this file and the DSP serves the hierarchy
 * again: the stock editor comes back, with the pad controls as notes.
 *
 * WHAT IT COSTS. The stock editor opens the host's file browser for a filepath cell; a module cannot reach that screen, so the
 * SID File cell opens a small list of its own here, built on the shared filepath_browser helpers.
 *
 * PADS. In Pad Mode Control (the pad_mode parameter) the pads are taken from the track and the bottom row is the controls:
 *   pad 1 2 3   mute / unmute voice 1, 2, 3       pad 4   unmute all
 *   pad 5 6 7   transpose down / up / back to 0
 *   pad 8       loop capture: press to start a loop on the bar playing (flashing red), again to end it on the bar playing then
 *               (steady red while it loops; very dark red when there is no loop); Loop Start and Loop Len follow
 *   second row  pad 1 2 3 switch Notes Out on and off for voice 1, 2, 3 (green sending, grey not), apart from the audio mutes
 * painted in a colour each (voices all blue, grey when muted; unmute white; transpose orange, yellow, red). The top row is a
 * little drum set of the tune's own sound effects (its other subtunes that sound once and stop, found by the DSP's scan):
 * purple where there is an effect, white for a moment when one is hit, dark where there is none; the harder the pad is hit,
 * the louder the effect. The rest of the grid is dark. In Play mode the pads are the track's and nothing is painted.
 */

import {
    createController, LAYOUT_MOVY, LAYOUT_LIST
} from '/data/UserData/schwung/shared/param_pages/page_controller.mjs';
import {
    decodeInput, applyInput, isHardwarePadPress
} from '/data/UserData/schwung/shared/param_pages/page_input.mjs';
import { frameCtx } from '/data/UserData/schwung/shared/param_pages/frame_ctx.mjs';
import { announce } from '/data/UserData/schwung/shared/screen_reader.mjs';
import { setLED, invalidateLedCache } from '/data/UserData/schwung/shared/input_filter.mjs';
import {
    buildFilepathBrowserState, refreshFilepathBrowser, moveFilepathBrowserSelection, activateFilepathBrowserItem
} from '/data/UserData/schwung/shared/filepath_browser.mjs';
import * as os from 'os';

/* The piano roll (canvas.js), inlined: it sets globalThis.canvas_overlay. */
/*@CANVAS@*/

/* The prefix WE choose. The controller asks for "<prefix>:<key>"; host_module_get_param already applies the real slot and
 * component prefix, so ours is stripped back off. */
const PREFIX = "tinysid";

/* The hierarchy the DSP will not serve while this file is installed. */
const HIERARCHY = JSON.stringify(@HIERARCHY@);

/* ------------------------------------------------------------------ pads -- */

const PAD_FIRST = 68;          /* the bottom-left pad; pads run left to right, then upward */
const PAD_COUNT = 32;
const CONTROL_PADS = 11;           /* the bottom row (7 controls; the eighth pad is dark) and the first three of the row above it */
const FX_PADS = 8;             /* the top row: pads 92..99 play the tune's sound effects */
const FX_FIRST_INDEX = PAD_COUNT - FX_PADS;
const FX_FLASH_FRAMES = 6;     /* a struck effect pad shows white for this many frames */
const POLL_TICKS = 6;          /* the pad mode and the controls' state are read this often (about 7 times a second) */
const LED_PER_FRAME = 8;       /* the first paint is spread over a few frames: the host's MIDI-out buffer refuses a burst */

/* Palette indexes (shared/constants.mjs): 33 blue, 119 dark grey, 120 white, 3 bright orange, 7 vivid yellow, 127 red. */
const COL_VOICE = [33, 33, 33];
const COL_MUTED = 119;
const COL_UNMUTE_ALL = 120;
const COL_DOWN = 3;
const COL_UP = 7;
const COL_RESET = 127;
const COL_LOOP_IDLE = 66;      /* very dark red: no loop */
const COL_LOOP_ON = 1;         /* bright red: looping, and the flash while a loop is being captured */
const LOOP_FLASH_FRAMES = 8;
const COL_MIDI_ON = 126;       /* green: Notes Out is sending this voice */
const COL_MIDI_OFF = 119;
const COL_FX = 22;             /* purple: an effect is there */
const COL_FX_HIT = 120;

let mode = 0;                  /* 0 Play, 1 Control: what pad_mode last read */
let mutes = 0;                 /* voice_mutes: bit v set = voice v muted */
let transpose = 0;
let midiOn = 0;                /* midi_out_v1..3: bit v set = voice v is sent as MIDI */
let loopCap = "idle";          /* loop_capture: "idle", "recording" or "looping" */
let loopFlash = 0;             /* frames, for the flash while recording */
let fxCount = 0;               /* fx_count: effects the DSP has found in the tune so far */
const fxFlash = new Array(FX_PADS).fill(0);
let padsPainted = false;
let paintCursor = 0;
let healCursor = 0;
let paintedKey = "";
let ticks = 0;

function padColour(i) {
    if (i < 3) return (mutes >> i) & 1 ? COL_MUTED : COL_VOICE[i];
    if (i === 3) return COL_UNMUTE_ALL;
    if (i === 4) return COL_DOWN;
    if (i === 5) return COL_UP;
    if (i === 6) return COL_RESET;
    if (i === 7) {
        if (loopCap === "recording") return Math.floor(loopFlash / LOOP_FLASH_FRAMES) % 2 === 0 ? COL_LOOP_ON : 0;
        return loopCap === "looping" ? COL_LOOP_ON : COL_LOOP_IDLE;
    }
    if (i >= 8 && i <= 10) return (midiOn >> (i - 8)) & 1 ? COL_MIDI_ON : COL_MIDI_OFF;
    if (i >= FX_FIRST_INDEX) {
        const e = i - FX_FIRST_INDEX;
        if (e >= fxCount) return 0;
        return fxFlash[e] > 0 ? COL_FX_HIT : COL_FX;
    }
    return 0;                  /* the other pads are dark while the pads are controls */
}

function readParam(key) {
    if (typeof host_module_get_param !== "function") return null;
    const v = host_module_get_param(key);
    return v === null || v === undefined || v === "" ? null : v;
}

function readInt(key, dflt) {
    const v = readParam(key);
    if (v === null) return dflt;
    const n = parseInt(v, 10);
    return isFinite(n) ? n : dflt;
}

function writeParam(key, value) {
    if (typeof host_module_set_param === "function") host_module_set_param(key, String(value));
}

/* The host never tells a module's screen that it has been left (a slot change, another editor), and the pad lights we painted stay
 * as they are. What it does do, every frame while a module does not own the pads, is call host_pad_block(0). So that call is wrapped
 * once: one that did not come from us, while our lights are up, hands the pads back as Move had them. Our own calls are marked.
 * The wrapper is installed once per script context and talks to whichever copy of this script loaded last. */
function padBlock(v) {
    if (typeof host_pad_block !== "function") return;
    globalThis.__tinysidOwnPadCall = true;
    try { host_pad_block(v); } finally { globalThis.__tinysidOwnPadCall = false; }
}

function installPadWatch() {
    if (typeof host_pad_block !== "function" || globalThis.__tinysidPadWatch) return;
    const original = host_pad_block;
    globalThis.__tinysidPadWatch = true;
    globalThis.host_pad_block = function (v) {
        const r = original.apply(this, arguments);
        try {
            const hook = globalThis.__tinysidPadHook;
            if (hook && !globalThis.__tinysidOwnPadCall) hook();
        } catch (e) { /* never let a clean-up get in the way of the host */ }
        return r;
    };
}

/* Repaint whatever Move believes it is showing. Outside overtake the shim's snapshot tracks MOVE's writes, so it is the state
 * to hand back; a note Move never lit has no cached colour, and dark is the truthful answer for it. */
function restoreMovePads() {
    const snap = (typeof shadow_get_pad_led_snapshot === "function") ? shadow_get_pad_led_snapshot() : null;
    invalidateLedCache();
    for (let note = PAD_FIRST; note < PAD_FIRST + PAD_COUNT; note++) {
        const c = snap && typeof snap[note] === "number" ? snap[note] : 0;
        setLED(note, c, true);
    }
    padsPainted = false;
    paintCursor = 0;
    globalThis.__tinysidLit = false;
}

/* The host called host_pad_block on its own account: if our lights are still up, the screen has been left. */
function padsHandedBack() {
    if (!globalThis.__tinysidLit) return;
    dbg("HANDBACK (the host took the pads)");
    restoreMovePads();
    mode = 0;
}

function paintPads() {
    globalThis.__tinysidLit = true;
    loopFlash++;
    const key = mutes + "|" + transpose + "|" + fxCount + "|" + midiOn + "|" + loopCap + "|" +
                (loopCap === "recording" ? Math.floor(loopFlash / LOOP_FLASH_FRAMES) % 2 : 0);
    if (!padsPainted) {
        const end = Math.min(paintCursor + LED_PER_FRAME, PAD_COUNT);
        for (let i = paintCursor; i < end; i++) setLED(PAD_FIRST + i, padColour(i));
        paintCursor = end;
        if (paintCursor >= PAD_COUNT) { padsPainted = true; paintedKey = key; }
        return;
    }
    if (key !== paintedKey) {
        for (let i = 0; i < CONTROL_PADS; i++) setLED(PAD_FIRST + i, padColour(i));
        for (let i = FX_FIRST_INDEX; i < PAD_COUNT; i++) setLED(PAD_FIRST + i, padColour(i));
        paintedKey = key;
    }
    for (let e = 0; e < FX_PADS; e++) {                 /* a struck effect pad goes back to its colour */
        if (fxFlash[e] > 0 && --fxFlash[e] === 0) setLED(PAD_FIRST + FX_FIRST_INDEX + e, padColour(FX_FIRST_INDEX + e));
    }
    repaintWhatMoveWrote();
    for (let k = 0; k < LED_PER_FRAME && crushLeft > 0; k++, crushLeft--) {   /* after a note event: the whole grid, a few pads a frame */
        setLED(PAD_FIRST + crushCursor, padColour(crushCursor), true);
        crushCursor = (crushCursor + 1) % PAD_COUNT;
    }
    /* one pad a frame is repainted regardless: that is what heals a clobber by Move's own writes */
    setLED(PAD_FIRST + healCursor, padColour(healCursor), true);
    healCursor = (healCursor + 1) % PAD_COUNT;
}

function enterControl() {
    dbg("MODE control");
    mode = 1;
    padsPainted = false;
    paintCursor = 0;
    paintedKey = "";
    fxFlash.fill(0);
    moveSeen.fill(-1);
    moveFight.fill(0);
    invalidateLedCache();
}

function leaveControl() {
    dbg("MODE play");
    mode = 0;
    restoreMovePads();
}

function pollState() {
    const m = readParam("pad_mode");
    const now = m === "Control" ? 1 : 0;
    if (now !== mode) { if (now) enterControl(); else leaveControl(); }
    mutes = readInt("voice_mutes", mutes);
    transpose = readInt("transpose", transpose);
    fxCount = Math.max(0, Math.min(FX_PADS, readInt("fx_count", fxCount)));
    let on = 0;
    for (let v = 0; v < 3; v++) if (readParam("midi_out_v" + (v + 1)) === "On") on |= 1 << v;
    midiOn = on;
    const lc = readParam("loop_capture");
    if (lc === "idle" || lc === "recording" || lc === "looping") loopCap = lc;
}

/* A control pad was pressed (note-on with velocity). The engine carries it out (pad_press in the DSP); the screen shows the result at
 * once, and its regular reads of the engine's state put right anything it guessed wrong. */
const POLL_HOLD_TICKS = 12;    /* after a press, the state is not read back for this many frames (about 0.4 s) */
let pollHold = 0;

function onPadPress(note, velocity) {
    const i = note - PAD_FIRST;
    if (i < 0 || i >= PAD_COUNT) return;
    dbg("PRESS " + note + "," + velocity);
    writeParam("pad_press", note + "," + Math.max(1, Math.min(127, velocity | 0 || 100)));
    /* Shown at once, as the engine will do it; the state polls are held off for a moment so a read that comes back before the
     * engine has caught up cannot flick the light back. */
    if (i < 3) { mutes ^= 1 << i; announce("Voice " + (i + 1) + ((mutes >> i) & 1 ? " muted" : " on")); }
    else if (i === 3) { mutes = 0; announce("All voices on"); }
    else if (i <= 6) { transpose = i === 6 ? 0 : Math.max(-12, Math.min(12, transpose + (i === 5 ? 1 : -1))); announce("Transpose " + transpose); }
    else if (i === 7) {
        loopCap = loopCap === "recording" ? "looping" : "recording";
        loopFlash = 0;
        announce(loopCap === "recording" ? "Loop capture started" : "Loop set");
    }
    else if (i >= 8 && i <= 10) { midiOn ^= 1 << (i - 8); announce("Voice " + (i - 7) + ((midiOn >> (i - 8)) & 1 ? " sent as MIDI" : " not sent")); }
    else if (i >= FX_FIRST_INDEX && i - FX_FIRST_INDEX < fxCount) {
        fxFlash[i - FX_FIRST_INDEX] = FX_FLASH_FRAMES;
        announce("Effect " + (i - FX_FIRST_INDEX + 1));
    } else return;
    setLED(note, padColour(i), true);
    pollHold = POLL_HOLD_TICKS;
}


/* The Move's firmware lights a pad when a note reaches it, over our colours (the notes Notes Out sends to the other tracks among them).
 * Two cures: any note event this screen sees schedules a repaint of the whole grid, a few pads a frame (ChordDex, MIT, does the same);
 * and every frame the shim's record of what MOVE last wrote to each pad is compared with the frame before: a pad Move has just written
 * gets our colour back at once. The second one works even when the notes never reach this screen, which on the device they do not. */
let crushLeft = 0;
let crushCursor = 0;
const moveSeen = new Array(PAD_COUNT).fill(-1);
const moveFight = new Array(PAD_COUNT).fill(0);
const FIGHT_FRAMES = 6;        /* our lights go through the host's rate-limited LED queue and Move's do not, so ours can land first:
                                  a pad Move wrote is repainted every frame for this long, so ours is the last word */

function repaintWhatMoveWrote() {
    if (typeof shadow_get_pad_led_snapshot !== "function") return;
    const snap = shadow_get_pad_led_snapshot();
    if (!snap) return;
    for (let i = 0; i < PAD_COUNT; i++) {
        const v = typeof snap[PAD_FIRST + i] === "number" ? snap[PAD_FIRST + i] : 0;
        if (moveSeen[i] >= 0 && v !== moveSeen[i]) moveFight[i] = FIGHT_FRAMES;
        moveSeen[i] = v;
        if (moveFight[i] > 0) { moveFight[i]--; setLED(PAD_FIRST + i, padColour(i), true); }
    }
}

function crushFirmwareLights(note) {
    crushLeft = PAD_COUNT;
    if (note >= PAD_FIRST && note < PAD_FIRST + PAD_COUNT) setLED(note, padColour(note - PAD_FIRST), true);
}

/* ------------------------------------------------------------- debug log -- */

/* Off unless the flag file exists (touch it over ssh, reload the module): then the screen records what it receives, what it lights,
 * what it hands the engine and what the engine did, and once a second which pads the Move itself has lit, into a file that is
 * rewritten every two seconds with the last DEBUG_LINES lines. The same pattern as the host's own overtake_midi_log_on. */
const DEBUG_FLAG = "/data/UserData/schwung/tmp/tinysid_debug_on";
const DEBUG_FILE = "/data/UserData/schwung/tmp/tinysid_debug.log";
const DEBUG_LINES = 4000;
let dbgOn = false;
let dbgLines = [];
let dbgDirty = false;
let dbgTick = 0;
let dbgT0 = 0;

function hex(a) {
    const out = [];
    for (let i = 0; a && i < a.length; i++) out.push(((a[i] & 255) < 16 ? "0" : "") + (a[i] & 255).toString(16));
    return out.join(" ");
}

function dbg(text) {
    if (!dbgOn) return;
    dbgLines.push((Date.now() - dbgT0) + " " + text);
    if (dbgLines.length > DEBUG_LINES) dbgLines.splice(0, dbgLines.length - DEBUG_LINES);
    dbgDirty = true;
}

function dbgInit() {
    dbgOn = typeof host_file_exists === "function" && typeof host_write_file === "function" && !!host_file_exists(DEBUG_FLAG);
    globalThis.__tinysidDbg = dbgOn ? dbg : null;
    if (dbgOn && !globalThis.__tinysidLedWrap && typeof move_midi_internal_send === "function") {
        const original = move_midi_internal_send;               /* every light we send goes through here */
        globalThis.__tinysidLedWrap = true;
        globalThis.move_midi_internal_send = function (pkt) {
            const r = original.apply(this, arguments);
            try { const h = globalThis.__tinysidDbg; if (h) h("OUT " + hex(pkt) + " -> " + r); } catch (e) { /* never in the way */ }
            return r;
        };
    }
    dbgT0 = Date.now();
    dbg("START");
}

function dbgTickWork() {
    if (!dbgOn) return;
    if ((dbgTick % 30) === 0) {
        const snap = typeof shadow_get_pad_led_snapshot === "function" ? shadow_get_pad_led_snapshot() : null;
        const lit = [];
        for (let note = PAD_FIRST; snap && note < PAD_FIRST + PAD_COUNT; note++) if (snap[note]) lit.push(note + ":" + snap[note]);
        dbg("STATE mode=" + mode + " mutes=" + mutes + " midiOn=" + midiOn + " fx=" + fxCount + " ack=" + readParam("pad_ack") +
            " midi_echoes=" + readParam("midi_echoes") + " out=" + readParam("mout_stats") + " delay=" + readParam("audio_delay") +
            " sent_lately=" + readParam("mout_echo") +
            " move_lit=" + lit.join(","));
    }
    if (dbgDirty && (dbgTick % 60) === 0) {
        try { host_write_file(DEBUG_FILE, dbgLines.join("\n") + "\n"); } catch (e) { /* nothing to do */ }
        dbgDirty = false;
    }
    dbgTick++;
}

/* ------------------------------------------------------------ the display -- */

function movy() {
    return {
        fillRect: fill_rect,
        print: print,
        textWidth: text_width,
        setPixel: set_pixel,
        line: typeof draw_line === "function" ? draw_line : undefined,
        fillCircle: typeof fill_circle === "function" ? fill_circle : undefined,
        drawCircle: typeof draw_circle === "function" ? draw_circle : undefined,
        drawArc: typeof draw_arc === "function" ? draw_arc : undefined,
    };
}

function stripPrefix(fullKey) {
    if (typeof fullKey !== "string") return "";
    return fullKey.startsWith(PREFIX + ":") ? fullKey.slice(PREFIX.length + 1) : fullKey;
}

const PARAM_VIEW_KNOBS = 1;

function layout() {
    try {
        if (typeof tts_get_enabled === "function" && tts_get_enabled()) return LAYOUT_LIST;
        if (typeof param_view_get_mode === "function") {
            return param_view_get_mode() === PARAM_VIEW_KNOBS ? LAYOUT_MOVY : LAYOUT_LIST;
        }
    } catch (e) { /* fall through to the grid */ }
    return LAYOUT_MOVY;
}

function shiftHeld() {
    return typeof shadow_get_shift_held === "function" && !!shadow_get_shift_held();
}

/* ------------------------------------------------------- the SID picker -- */

/* The host's own file browser belongs to its editor and a module cannot open it, so the SID File cell opens this: the same
 * shared state machine (folders, the .sid filter, a start folder), a plain list to look at. Jog scrolls, click opens a folder
 * or picks a file, Back closes it. */
const FILEPATH_FS = {
    readdir(path) {
        const entries = os.readdir(path) || [];
        if (Array.isArray(entries[0])) return entries[0];
        return Array.isArray(entries) ? entries : [];
    },
    stat(path) { return os.stat(path); }
};

let picker = null;             /* { state, key } while the list is up */

function openPicker(key, meta) {
    const current = readParam(key) || "";
    const state = buildFilepathBrowserState(meta || { key }, current);
    refreshFilepathBrowser(state, FILEPATH_FS);
    picker = { state, key };
    announce(state.items.length ? (state.items[state.selectedIndex].label || "File") : "No files");
}

function closePicker() {
    picker = null;
}

function pickerInput(data) {
    const intent = decodeInput(data, {});
    if (!intent) return;
    if (intent.type === "page") {
        moveFilepathBrowserSelection(picker.state, intent.delta);
        const it = picker.state.items[picker.state.selectedIndex];
        if (it) announce(it.label || "File");
    } else if (intent.type === "click") {
        const res = activateFilepathBrowserItem(picker.state);
        if (res.action === "open") {
            refreshFilepathBrowser(picker.state, FILEPATH_FS);
            const it = picker.state.items[picker.state.selectedIndex];
            if (it) announce(it.label || "Folder");
        } else if (res.action === "select") {
            const key = picker.key;
            closePicker();
            if (ctl && typeof ctl.commitValue === "function") ctl.commitValue(key, res.value);
            else writeParam(key, res.value);
            announce(res.filename || "Loaded");
        }
    } else if (intent.type === "back") {
        closePicker();
    }
}

function drawPicker() {
    clear_screen();
    const st = picker.state;
    fill_rect(0, 0, 128, 11, 1);
    print(2, 2, String(st.title || "File").slice(0, 20), 0);
    const rows = 4, top = 14, h = 9;
    const n = st.items.length;
    if (!n) {
        print(4, top + 4, st.error ? String(st.error).slice(0, 20) : "No files", 1);
    } else {
        let first = Math.max(0, Math.min(st.selectedIndex - 1, n - rows));
        for (let r = 0; r < rows && first + r < n; r++) {
            const idx = first + r, y = top + r * h, sel = idx === st.selectedIndex;
            if (sel) fill_rect(0, y - 1, 128, h, 1);
            print(3, y, String(st.items[idx].label).slice(0, 20), sel ? 0 : 1);
        }
    }
    const it = n ? st.items[st.selectedIndex] : null;
    print(2, 54, it && it.kind === "file" ? "Click: pick  Back: close" : "Click: open  Back: close", 1);
}

/* -------------------------------------------------------------- lifecycle -- */

let ctl = null;
let landed = false;
let contractTick = 0;
const CONTRACT_TICKS = 32;     /* ~0.7 s: reloadIfChanged reads chain_params over IPC, so it is not a per-frame call */

function buildController() {
    ctl = createController({
        getParam(fullKey) {
            const key = stripPrefix(fullKey);
            if (key === "ui_hierarchy") return HIERARCHY;     /* the one key the DSP refuses, answered from here */
            if (typeof host_module_get_param !== "function") return null;
            return host_module_get_param(key);
        },
        setParam(fullKey, value) {
            const key = stripPrefix(fullKey);
            if (typeof host_module_set_param === "function") host_module_set_param(key, String(value));
        },
        announce,
        /* "My Presets" and "Module", bound by the host with our slot and component already applied */
        trailingMenus() {
            return (typeof shadow_component_trailing_menus === "function") ? shadow_component_trailing_menus() : [];
        },
        /* the piano roll */
        drawCanvasPage(drawCtx, band, canvas, payload) {
            const overlay = globalThis.canvas_overlay;
            if (overlay && typeof overlay.drawPage === "function") overlay.drawPage(frameCtx(drawCtx, band), payload || {});
        },
    });
    ctl.load({ slot: 0, component: PREFIX, prefix: PREFIX });
    ctl.setLayout(layout());
    landOnRoll();
}

/* Open on the piano roll, whatever order the planner emitted the pages in. An outcome, not an event: it retries every tick
 * until it has succeeded once (a failed first read leaves the controller with no pages yet). */
function landOnRoll() {
    if (landed || !ctl) return;
    const pages = ctl.pages || [];
    for (let i = 0; i < pages.length; i++) {
        if (pages[i] && pages[i].canvas) { ctl.goToPage(i); landed = true; return; }
    }
}

function init() {
    dbgInit();
    installPadWatch();
    globalThis.__tinysidPadHook = padsHandedBack;
    mode = 0;
    mutes = 0;
    transpose = 0;
    fxCount = 0;
    midiOn = 0;
    loopCap = "idle";
    pollHold = 0;
    crushLeft = 0;
    offScreen = false;
    fxFlash.fill(0);
    padsPainted = false;
    paintCursor = 0;
    healCursor = 0;
    paintedKey = "";
    ticks = 0;
    contractTick = 0;
    landed = false;
    picker = null;
    invalidateLedCache();
    buildController();
    pollState();
}

/* The host's own test for "is our screen actually up": its view survives Move taking the display back (a track change, Menu),
 * and so does this script's tick. While Move is on screen the pads and their lights are Move's. */
let offScreen = false;

function onScreen() {
    return typeof shadow_get_display_mode !== "function" || shadow_get_display_mode() === 1;
}

function tick() {
    if (!ctl) return;
    if (!onScreen()) {
        if (!offScreen) {
            offScreen = true;
            dbg("OFFSCREEN (Move has the display)");
            if (globalThis.__tinysidLit) restoreMovePads();
        }
        padBlock(0);
        return;
    }
    if (offScreen) {                                   /* back on screen: the control lights are painted afresh */
        offScreen = false;
        dbg("ONSCREEN");
        padsPainted = false;
        paintCursor = 0;
        paintedKey = "";
        invalidateLedCache();
    }
    if (pollHold > 0) { pollHold--; ticks++; }
    else if ((ticks++ % POLL_TICKS) === 0) pollState();
    /* RESTATED every frame, never edged: the shim drops pad_block on its own at display-mode edges, so a mirror of it would
     * latch and leave the pads dead. Control mode takes the pads; Play mode gives them back to the track. */
    padBlock(mode === 1 ? 1 : 0);
    if (mode === 1) paintPads();
    dbgTickWork();

    if (picker) { drawPicker(); return; }

    if ((contractTick++ % CONTRACT_TICKS) === 0) ctl.reloadIfChanged();
    ctl.tick();
    landOnRoll();

    const ctx = movy();
    clear_screen();
    ctl.render(ctx, { title: "TinySID" });
    /* The library never clears the screen, so anything full-screen (the enum peek) comes back to us. */
    ctl.renderOverlays(ctx, { clearScreen: clear_screen });
}

function onMidiMessageInternal(data) {
    if (!ctl) return;
    if (picker) { pickerInput(data); return; }

    /* Pads first: decodeInput knows the eight knob-touch notes and nothing else in the note range. Only a blocked pad
     * (Control mode) is delivered here at all. */
    if (dbgOn && data && data.length >= 1 && data[0] !== 0xF8 && data[0] !== 0xFE) dbg("IN " + hex(data));
    if (mode === 1 && data && data.length >= 3 && (data[0] & 0xE0) === 0x80) crushFirmwareLights(data[1]);   /* any note on or off */
    if (isHardwarePadPress(data)) { if (mode === 1) onPadPress(data[1], data[2]); return; }

    const intent = decodeInput(data, { shift: shiftHeld() });
    if (!intent) return;
    const todo = applyInput(ctl, intent, { nowMs: Date.now() });
    if (!todo) return;

    if (todo.action === "menu" && todo.entry && todo.entry.action) {
        if (typeof shadow_component_run_action === "function") shadow_component_run_action(todo.entry.action);
        return;
    }
    if (todo.action === "open") {
        const meta = todo.meta || {};
        if (meta.type === "filepath" || meta.kind === "filepath" || todo.key === "sid_file") openPicker(todo.key || "sid_file", meta);
        /* an enum needs no picker: turning its knob cycles the options and the controller shows the list while you turn */
    }
}

/* Back is offered to us first. Consume it only while there is somewhere to go back TO: the list closes; otherwise the pads go
 * back to Move and the host leaves. */
function handleBack() {
    if (picker) { closePicker(); return true; }
    padBlock(0);
    if (mode === 1) restoreMovePads();
    return false;
}

/* Pure helpers, exported for the tests. The host never looks at this. */
globalThis.chain_ui_test = {
    HIERARCHY, padColour, openPicker, controlState: () => ({ mode, mutes, transpose }), picker: () => picker, controller: () => ctl,
};

/* The host swallows an exception thrown here and the screen just stops responding, which says nothing about why. So every entry
 * point is guarded, and what it threw is drawn along the bottom of the display for a few seconds. */
let fault = "";
let faultUntil = 0;

function guarded(name, fn, fallback) {
    return function (...args) {
        try {
            return fn.apply(null, args);
        } catch (e) {
            fault = name + ": " + String((e && e.message) || e);
            faultUntil = Date.now() + 8000;
            if (typeof console !== "undefined" && console.log) console.log("TinySID ui_chain " + fault);
            return fallback;
        }
    };
}

function drawFault() {
    if (!fault || Date.now() > faultUntil) return;
    fill_rect(0, 52, 128, 12, 1);
    print(1, 54, fault.slice(0, 21), 0);
}

const guardedTick = guarded("tick", tick);
globalThis.chain_ui = {
    init: guarded("init", init),
    tick() { guardedTick(); drawFault(); },
    onMidiMessageInternal: guarded("midi", onMidiMessageInternal),
    handleBack: guarded("back", handleBack, false),
};
