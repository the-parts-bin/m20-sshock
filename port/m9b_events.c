// System Shock on the m9b SoC: keyboard and mouse, in place of
// Libraries/INPUT/Source/sdl_events.c (the same queues and functions:
// pump_events, kb_*, mouse_next and the rest).
//
// Keys: USB keyboards (../../core/rtl/usb_port.v, polled) report HID usage
// codes; hid_mac[] turns them into the Mac virtual key codes the game
// works with (sdl_events.c's sdlKeyCodeToSSHOCKkeyCode). Events carry the
// character as sdl_events.c made them: printable keys without Ctrl or Alt
// come down as their (shifted) character, the rest with their unshifted
// character or the invented codes 128+n of MacSrc/Prefs.c's hotkeys.
// Keys from the UART (run.py's terminal): arrows, Enter, Esc, Backspace,
// printable characters (and Ctrl-E: email 1, for tests); a terminal only sends presses, so a UART key
// counts as held until 150 ms after its last repeat.
//
// Mouse: the hardware's running sums of motion (one per port), buttons
// from the last report. The pointer moves over the screen in its pixels
// (MouseX/Y); mouselook reads the motion as relative velocities.
#include <stdint.h>
#include <string.h>

#include "lg.h"
#include "kb.h"
#include "mouse.h"
#include "SDL.h"
#include "m9b_map.h"

#define UART_DATA     (*(volatile uint32_t*)M9B_UART)
#define UART_RX_VALID (1u << 8)

extern uint32_t TickCount(void);
extern void m9b_screenshot(void);    // m9b_shock.c
extern void m9b_sound_poll(void);    // m9b_sound.c
extern int32_t gScreenWide, gScreenHigh;

bool fullscreenActive = false;

// sdl_events.c's state: key states by Mac code, the two event queues.
uchar sshockKeyStates[256];
enum { kNumKBevents = 128, kNumMouseEvents = 128 };
static kbs_event kbEvents[kNumKBevents];
static int nextKBevent = 0;
static ss_mouse_event mouseEvents[kNumMouseEvents];
static int nextMouseEvent = 0;
ss_mouse_event latestMouseEvent;

int MouseX, MouseY;
int MouseChaosX, MouseChaosY;
extern bool TriggerRelMouseMode;
void get_mouselook_vel(int *vx, int *vy);

static void addKBevent(const kbs_event *ev) {
    if (nextKBevent < kNumKBevents) {
        kbEvents[nextKBevent++] = *ev;
    } else {   // full: drop the oldest
        memmove(&kbEvents[0], &kbEvents[1], sizeof(kbs_event) * (kNumKBevents - 1));
        kbEvents[kNumKBevents - 1] = *ev;
    }
}

static void addMouseEvent(const ss_mouse_event *ev) {
    latestMouseEvent = *ev;
    if (nextMouseEvent < kNumMouseEvents) {
        mouseEvents[nextMouseEvent++] = *ev;
    } else {
        memmove(&mouseEvents[0], &mouseEvents[1], sizeof(ss_mouse_event) * (kNumMouseEvents - 1));
        mouseEvents[kNumMouseEvents - 1] = *ev;
    }
}

// -------------------------------------------------------------------------
// Keys
// -------------------------------------------------------------------------
#define HID_LCTRL  0xE0
#define HID_LSHIFT 0xE1
#define HID_LALT   0xE2
#define HID_RCTRL  0xE4
#define HID_RSHIFT 0xE5
#define HID_RALT   0xE6
#define HID_RETURN 0x28
#define HID_ESCAPE 0x29
#define HID_BACKSP 0x2A
#define HID_RIGHT  0x4F
#define HID_LEFT   0x50
#define HID_DOWN   0x51
#define HID_UP     0x52

// HID usage -> Mac virtual key code (KBC_NONE: not used).
static const uint8_t hid_mac[0x64] = {
    [0x00 ... 0x63] = KBC_NONE,
    [0x04] = 0x00, [0x05] = 0x0B, [0x06] = 0x08, [0x07] = 0x02, [0x08] = 0x0E, [0x09] = 0x03,   // a..f
    [0x0A] = 0x05, [0x0B] = 0x04, [0x0C] = 0x22, [0x0D] = 0x26, [0x0E] = 0x28, [0x0F] = 0x25,   // g..l
    [0x10] = 0x2E, [0x11] = 0x2D, [0x12] = 0x1F, [0x13] = 0x23, [0x14] = 0x0C, [0x15] = 0x0F,   // m..r
    [0x16] = 0x01, [0x17] = 0x11, [0x18] = 0x20, [0x19] = 0x09, [0x1A] = 0x0D, [0x1B] = 0x07,   // s..x
    [0x1C] = 0x10, [0x1D] = 0x06,                                                               // y z
    [0x1E] = 0x12, [0x1F] = 0x13, [0x20] = 0x14, [0x21] = 0x15, [0x22] = 0x17, [0x23] = 0x16,   // 1..6
    [0x24] = 0x1A, [0x25] = 0x1C, [0x26] = 0x19, [0x27] = 0x1D,                                 // 7..0
    [0x28] = 0x24, [0x29] = 0x35, [0x2A] = 0x33, [0x2B] = 0x30, [0x2C] = 0x31,   // Enter Esc BS Tab Space
    [0x2D] = 0x1B, [0x2E] = 0x18, [0x2F] = 0x21, [0x30] = 0x1E, [0x31] = 0x2A,   // - = [ ] backslash
    [0x33] = 0x29, [0x34] = 0x27, [0x35] = 0x32, [0x36] = 0x2B, [0x37] = 0x2F, [0x38] = 0x2C,   // ; ' ` , . /
    [0x3A] = 0x7A, [0x3B] = 0x78, [0x3C] = 0x63, [0x3D] = 0x76, [0x3E] = 0x60, [0x3F] = 0x61,   // F1..F6
    [0x40] = 0x62, [0x41] = 0x64, [0x42] = 0x65, [0x43] = 0x6D, [0x44] = 0x67, [0x45] = 0x6F,   // F7..F12
    [0x49] = 0x75, [0x4A] = 0x73, [0x4B] = 0x74, [0x4C] = 0x33, [0x4D] = 0x77, [0x4E] = 0x79,   // Ins Home PgUp Del End PgDn
    [0x4F] = 0x7C, [0x50] = 0x7B, [0x51] = 0x7D, [0x52] = 0x7E,                                 // arrows
    [0x54] = 0x4B, [0x55] = 0x43, [0x56] = 0x4E, [0x57] = 0x45, [0x58] = 0x4C,                  // keypad / * - + Enter
    [0x59] = 0x53, [0x5A] = 0x54, [0x5B] = 0x55, [0x5C] = 0x56, [0x5D] = 0x57, [0x5E] = 0x58,   // keypad 1..6
    [0x5F] = 0x59, [0x60] = 0x5B, [0x61] = 0x5C, [0x62] = 0x52, [0x63] = 0x41,                  // keypad 7 8 9 0 .
};

// The characters of HID usages 0x04..0x38, unshifted and shifted.
static const char plain[] = "\0\0\0\0abcdefghijklmnopqrstuvwxyz1234567890\r\x1b\b\t -=[]\\\0;'`,./";
static const char shifted[] = "\0\0\0\0ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\r\x1b\b\t _+{}|\0:\"~<>?";

// The "character" of a key with no text (Prefs.c's hotkeys): SDL keycodes
// for Delete and the keypad, 128+n for function and navigation keys.
static int key_char(int hid) {
    if (hid >= 0x04 && hid <= 0x38) return (uint8_t)plain[hid];
    if (hid >= 0x3A && hid <= 0x45) return 128 + (hid - 0x3A);   // F1..F12
    switch (hid) {
    case 0x4C: return 127;        // Delete
    case 0x54: return 128 + 12;   // keypad /
    case 0x55: return 128 + 13;   // keypad *
    case 0x56: return 128 + 14;   // keypad -
    case 0x57: return 128 + 15;   // keypad +
    case 0x58: return 128 + 16;   // keypad Enter
    case 0x63: return 128 + 17;   // keypad .
    case 0x62: return 128 + 18;   // keypad 0
    case 0x4A: return 128 + 19;   // Home
    case 0x4B: return 128 + 20;   // Page Up
    case 0x49: return 128 + 21;   // Insert
    case 0x4D: return 128 + 23;   // End
    case 0x4E: return 128 + 25;   // Page Down
    }
    return 0;
}

static uint8_t mods;   // held modifiers, HID bits (0 LCtrl, 1 LShift, 2 LAlt, 4 RCtrl, 5 RShift, 6 RAlt)

static int mod_shift(void) { return (mods & 0x22u) != 0; }
static int mod_ctrl(void) { return (mods & 0x11u) != 0; }
static int mod_alt(void) { return (mods & 0x44u) != 0; }

int SDL_GetModState(void) {
    return (mod_shift() ? KMOD_SHIFT : 0) | (mod_ctrl() ? KMOD_CTRL : 0) | (mod_alt() ? KMOD_ALT : 0);
}

static void key_event(int hid, int down) {
    if (hid >= 0xE0 && hid <= 0xE7) {   // modifiers
        uint8_t bit = (uint8_t)(1u << (hid - 0xE0));
        mods = down ? mods | bit : mods & ~bit;
        if (hid == HID_LSHIFT || hid == HID_RSHIFT) {   // shift after a move key (sdl_events.c's hack)
            for (int i = 0; i < 256; i++)
                if (sshockKeyStates[i]) {
                    if (down) sshockKeyStates[i] |= KB_MOD_SHIFT;
                    else sshockKeyStates[i] &= ~KB_MOD_SHIFT;
                }
        }
        return;
    }
    if (hid >= (int)sizeof(hid_mac) || hid_mac[hid] == KBC_NONE) return;
    kbs_event ev = {0};
    ev.code = hid_mac[hid];
    ev.ascii = (uchar)key_char(hid);
    if (mod_shift()) ev.modifiers |= KB_MOD_SHIFT;
    if (mod_ctrl()) ev.modifiers |= KB_MOD_CTRL;
    if (mod_alt()) ev.modifiers |= KB_MOD_ALT;
    if (!down) {
        ev.state = KBS_UP;
        addKBevent(&ev);
        sshockKeyStates[ev.code] = 0;
        return;
    }
    ev.state = KBS_DOWN;
    if (hid >= 0x04 && hid <= 0x38 && hid != HID_RETURN && hid != HID_ESCAPE && hid != HID_BACKSP && hid != 0x2B &&
        !mod_ctrl() && !mod_alt()) {
        // Text (sdl_events.c's SDL_TEXTINPUT): the shifted character,
        // capitals as lower case with Shift.
        int ch = (uint8_t)(mod_shift() ? shifted[hid] : plain[hid]);
        if (!ch) return;
        ev.modifiers = 0;
        if (ch >= 'A' && ch <= 'Z') {
            ch += 'a' - 'A';
            ev.modifiers |= KB_MOD_SHIFT;
        }
        ev.ascii = (uchar)ch;
    }
    addKBevent(&ev);
    sshockKeyStates[ev.code] = ev.modifiers | KB_MOD_PRESSED;
}

// -------------------------------------------------------------------------
// UART keys
// -------------------------------------------------------------------------
#define HOLD_MS 150u
static struct { int key; uint32_t until; } held[8];
static uint32_t esc_state, esc_time;   // 0, 1 after ESC, 2 after ESC [

static void uart_key(int hid, int shift) {
    uint32_t now = SDL_GetTicks();
    for (int i = 0; i < 8; i++)
        if (held[i].key == hid) { held[i].until = now + HOLD_MS; return; }
    for (int i = 0; i < 8; i++)
        if (!held[i].key) {
            held[i].key = hid;
            held[i].until = now + HOLD_MS;
            if (shift) key_event(HID_LSHIFT, 1);
            key_event(hid, 1);
            if (shift) key_event(HID_LSHIFT, 0);
            return;
        }
}

static int hid_of_char(unsigned char c, int *shift) {
    *shift = 0;
    for (int k = 4; k <= 0x38; k++) {
        if (k >= 0x28 && k <= 0x2B) continue;   // Enter, Esc, Backspace, Tab: by code
        if (plain[k] && plain[k] == (char)c) return k;
        if (shifted[k] && shifted[k] == (char)c) { *shift = 1; return k; }
    }
    return 0;
}

static void poll_uart(void) {
    uint32_t now = SDL_GetTicks();
    if (esc_state && now - esc_time > 30u) { esc_state = 0; uart_key(HID_ESCAPE, 0); }
    for (;;) {
        uint32_t d = UART_DATA;
        if (!(d & UART_RX_VALID)) break;
        unsigned char c = (unsigned char)d;
        if (esc_state == 1) {
            esc_state = c == '[' ? 2u : 0u;
            if (!esc_state) uart_key(HID_ESCAPE, 0);
            continue;
        }
        if (esc_state == 2) {
            esc_state = 0;
            if (c == 'A') uart_key(HID_UP, 0);
            else if (c == 'B') uart_key(HID_DOWN, 0);
            else if (c == 'C') uart_key(HID_RIGHT, 0);
            else if (c == 'D') uart_key(HID_LEFT, 0);
            continue;
        }
        if (c == 27) { esc_state = 1; esc_time = now; }
        else if (c == 0x1Cu) m9b_screenshot();
#ifdef M20_ENGINE
        else if (c == 0x04u) {   // Ctrl-D (debugging): the 8 bpp screen as the game drew it
            extern void m20_screenshot_screen(void);
            m20_screenshot_screen();
        }
        else if (c == 0x0Bu) {   // Ctrl-K: the span engine off and on (port/m20_engine.c)
            extern int m20_engine_enable;
            m20_engine_enable = (m20_engine_enable + 1) & 3;   // all, walls only, floors and the rest, off
            static const char *const what[4] = {"off", "on", "on for the walls", "on for floors and the rest"};
            printf("span engine %s\n", what[m20_engine_enable]);
        }
        else if (c == 0x12u) {   // Ctrl-R (tests): the span engine's rows from the CPU or the EDGE command
            extern int m20_engine_rows;
            m20_engine_rows = !m20_engine_rows;
            printf("span engine rows: %s\n", m20_engine_rows ? "the CPU's" : "EDGE");
        }
#endif
        else if (c == 0x0Fu) {   // Ctrl-O (tests): the on-line help off and on, not saved in the prefs
            extern uchar olh_active;
            olh_active = !olh_active;
            printf("uart: on-line help %s\n", olh_active ? "on" : "off");
        }
        else if (c == 0x05u) {   // Ctrl-E: opens email 1 (tests: tools/newgame.sh)
            extern void read_email(int new_base, int num);
            printf("uart: email 1\n");
            read_email(0x989, 1);   // EMAIL_BASE_ID (RES_email0)
        }
        else if (c == '\r' || c == '\n') uart_key(HID_RETURN, 0);
        else if (c == 8 || c == 127) uart_key(HID_BACKSP, 0);
        else if (c == '\t') uart_key(0x2B, 0);
        else if (c >= 32 && c < 127) {
            int shift, k = hid_of_char(c, &shift);
            if (k) uart_key(k, shift);
        }
    }
    for (int i = 0; i < 8; i++)
        if (held[i].key && (int32_t)(now - held[i].until) >= 0) {
            key_event(held[i].key, 0);
            held[i].key = 0;
        }
}

// -------------------------------------------------------------------------
// Mouse
// -------------------------------------------------------------------------
static int rel_mode;           // mouselook: motion is read as velocity
static int rel_dx, rel_dy;     // motion since the last get_mouselook_vel
static uint8_t buttons;        // HID bits: 0 left, 1 right, 2 middle

static void clamp_mouse(void) {
    if (MouseX < 0) MouseX = 0;
    if (MouseY < 0) MouseY = 0;
    if (MouseX > gScreenWide - 1) MouseX = gScreenWide - 1;
    if (MouseY > gScreenHigh - 1) MouseY = gScreenHigh - 1;
}

static uchar event_buttons(void) {
    return (uchar)((buttons & 1u ? 1u << MOUSE_LBUTTON : 0u) | (buttons & 2u ? 1u << MOUSE_RBUTTON : 0u));
}

static void mouse_motion(int dx, int dy) {
    rel_dx += dx;
    rel_dy += dy;
    MouseX += dx;
    MouseY += dy;
    clamp_mouse();
    ss_mouse_event ev = {0};
    ev.type = MOUSE_MOTION;
    ev.x = (short)MouseX;
    ev.y = (short)MouseY;
    ev.buttons = event_buttons();
    ev.timestamp = mouse_get_time();
    addMouseEvent(&ev);
    if (TriggerRelMouseMode) {
        TriggerRelMouseMode = FALSE;
        SDL_SetRelativeMouseMode(SDL_TRUE);
        int vx, vy;
        get_mouselook_vel(&vx, &vy);   // throw away the first reading
    }
}

static void mouse_button_at(int b, int down, uint32_t when) {   // b: HID bit; when: TickCount()
    static const uint16_t down_type[3] = {MOUSE_LDOWN, MOUSE_RDOWN, MOUSE_CDOWN};
    static const uint16_t up_type[3] = {MOUSE_LUP, MOUSE_RUP, MOUSE_CUP};
    buttons = down ? buttons | (uint8_t)(1u << b) : buttons & (uint8_t)~(1u << b);
    ss_mouse_event ev = {0};
    ev.type = down ? down_type[b] : up_type[b];
    ev.buttons = event_buttons();
    ev.x = (short)MouseX;
    ev.y = (short)MouseY;
    ev.timestamp = when;
    ev.modifiers = mod_shift() ? 1 : 0;
    addMouseEvent(&ev);
}

static void mouse_button(int b, int down) { mouse_button_at(b, down, mouse_get_time()); }

// Button changes, sampled in the audio interrupt (m9b_mouse_sample, every
// 5.3 ms: faster than a mouse's reports) with their time: the main loop
// polls once a frame, 100 ms or more apart on the board, so a quick
// double click (down, up, down within about 140 ms by default) used to fall
// between two polls and the UI never saw it. usb_poll turns the queue
// into button events with those times.
#define BQ 32u
static volatile uint8_t bq_state[BQ];    // the port (bit 4) and its buttons
static volatile uint32_t bq_time[BQ];
static volatile uint32_t bq_w, bq_r;

// The time (TickCount) up to which the button changes are in the game's
// queue: uiPoll (Libraries/UI/Source/event.c, M9B) expires a pending click
// at this time rather than at the current one. The main loop polls the UI
// before it pumps the events, a whole frame (100 ms or more) after the
// last pump, so at the current time a first click could expire while the
// second, made within the double-click time, still waited in bq[].
uint32_t m9b_input_time;


Uint32 SDL_GetMouseState(int *x, int *y) {
    if (x) *x = MouseX;
    if (y) *y = MouseY;
    return (buttons & 1u ? SDL_BUTTON(SDL_BUTTON_LEFT) : 0u) | (buttons & 2u ? SDL_BUTTON(SDL_BUTTON_RIGHT) : 0u) |
           (buttons & 4u ? SDL_BUTTON(SDL_BUTTON_MIDDLE) : 0u);
}

int SDL_SetRelativeMouseMode(SDL_bool on) {
    rel_mode = on;
    return 0;
}

SDL_bool SDL_GetRelativeMouseMode(void) { return rel_mode ? SDL_TRUE : SDL_FALSE; }

void SDL_WarpMouseInWindow(SDL_Window *w, int x, int y) {
    (void)w;
    MouseX = x;
    MouseY = y;
    clamp_mouse();
}

int SDL_ShowCursor(int toggle) { return toggle == SDL_QUERY ? SDL_DISABLE : toggle; }

// -------------------------------------------------------------------------
// USB keyboards and mice (see ../../doom/port/doomgeneric_m9b.c)
// -------------------------------------------------------------------------
#define USB_REG(p, o)    (*(volatile uint32_t*)(M9B_USB + 0x20u * (p) + (o)))
#define USB_STATUS(p)    USB_REG(p, 0x00u)   // bits 1:0 type, 15:8 report count
#define USB_KEYS(p)      USB_REG(p, 0x04u)   // {k3, k2, k1, modifiers}
#define USB_KEYS2(p)     USB_REG(p, 0x08u)   // k4
#define USB_MOUSE(p)     USB_REG(p, 0x0Cu)   // {dy, dx, buttons}
#define USB_MOUSE_POS(p) USB_REG(p, 0x14u)   // {y, x}, 16-bit running sums

static struct {
    uint32_t type, count;
    uint8_t keys[4], mods, buttons;
    uint32_t pos;
} usb[2];

static int key_in(uint8_t k, const uint8_t *keys) {
    for (int i = 0; i < 4; i++)
        if (keys[i] == k) return 1;
    return 0;
}

static void usb_keys_changed(const uint8_t *old_keys, uint8_t old_mods, const uint8_t *keys, uint8_t new_mods) {
    for (int b = 0; b < 8; b++)   // modifier bits = HID usages 0xE0..0xE7
        if (((old_mods ^ new_mods) >> b) & 1u) key_event(0xE0 + b, (new_mods >> b) & 1);
    for (int i = 0; i < 4; i++)
        if (old_keys[i] > 3u && !key_in(old_keys[i], keys)) key_event(old_keys[i], 0);
    for (int i = 0; i < 4; i++)
        if (keys[i] > 3u && !key_in(keys[i], old_keys)) key_event(keys[i], 1);
}

void m9b_mouse_sample(void) {   // from the audio interrupt
    static uint8_t last[2];
    for (uint32_t p = 0; p < 2u; p++) {
        if ((USB_STATUS(p) & 3u) != 2u) continue;
        uint8_t b = (uint8_t)(USB_MOUSE(p) & 7u);
        if (b == last[p]) continue;
        last[p] = b;
        if (bq_w - bq_r < BQ) {
            bq_state[bq_w % BQ] = (uint8_t)(b | p << 4);
            bq_time[bq_w % BQ] = TickCount();
            bq_w++;
        }
    }
}

static void usb_poll(void) {
    static const uint8_t none[4];
    uint32_t input_time = TickCount();   // before draining bq[]: all changes until then get drained
    for (uint32_t p = 0; p < 2u; p++) {
        uint32_t st = USB_STATUS(p), type = st & 3u, count = st >> 8 & 0xFFu;
        if (type != usb[p].type) {   // unplugged or replaced: release everything
            usb_keys_changed(usb[p].keys, usb[p].mods, none, 0);
            for (int b = 0; b < 3; b++)
                if ((usb[p].buttons >> b) & 1u) mouse_button(b, 0);
            memset(usb[p].keys, 0, 4);
            usb[p].mods = usb[p].buttons = 0;
            usb[p].pos = USB_MOUSE_POS(p);
            usb[p].type = type;
            usb[p].count = count;
            continue;
        }
        if (type == 2u) {
            uint32_t pos = USB_MOUSE_POS(p);
            int dx = (int16_t)(pos - usb[p].pos), dy = (int16_t)((pos >> 16) - (usb[p].pos >> 16));
            usb[p].pos = pos;
            if (dx || dy) mouse_motion(dx, dy);
            while (bq_r != bq_w) {   // the interrupt's button changes, with their times
                uint8_t e = bq_state[bq_r % BQ];
                uint32_t when = bq_time[bq_r % BQ];
                bq_r++;
                if (when - input_time < 0x80000000u) input_time = when;   // drained, so also covered
                if ((e >> 4) != p) continue;
                for (int i = 0; i < 3; i++)
                    if ((((e & 7u) ^ usb[p].buttons) >> i) & 1u) mouse_button_at(i, (e >> i) & 1, when);
                usb[p].buttons = e & 7u;
            }
            uint8_t b = (uint8_t)(USB_MOUSE(p) & 7u);   // (without the interrupt: once a frame)
            for (int i = 0; i < 3; i++)
                if (((b ^ usb[p].buttons) >> i) & 1u) mouse_button(i, (b >> i) & 1);
            usb[p].buttons = b;
            continue;
        }
        if (type != 1u || count == usb[p].count) continue;
        usb[p].count = count;
        uint32_t k = USB_KEYS(p);
        uint8_t keys[4] = {(uint8_t)(k >> 8), (uint8_t)(k >> 16), (uint8_t)(k >> 24), (uint8_t)USB_KEYS2(p)};
        if (keys[0] == 0x01u) continue;   // ErrorRollOver
        usb_keys_changed(usb[p].keys, usb[p].mods, keys, (uint8_t)k);
        memcpy(usb[p].keys, keys, 4);
        usb[p].mods = (uint8_t)k;
    }
    m9b_input_time = input_time;
}

void pump_events(void) {
    poll_uart();
    usb_poll();
    m9b_sound_poll();
}

// -------------------------------------------------------------------------
// kbMac.c's interface (as in sdl_events.c)
// -------------------------------------------------------------------------
int pKbdStatusFlags;

int kb_startup(void *v) {
    (void)v;
    pKbdStatusFlags = 0;
    memset(sshockKeyStates, 0, sizeof(sshockKeyStates));
    nextKBevent = 0;
    return 0;
}

int kb_shutdown(void) { return 0; }
int kb_get_flags() { return pKbdStatusFlags; }
void kb_set_flags(int flags) { pKbdStatusFlags = flags; }

kbs_event kb_look_next(void) {
    kbs_event ev = {0xFF, 0x00};
    if (kb_get_flags() & KBF_BLOCK)
        while (nextKBevent == 0) pump_events();
    if (nextKBevent > 0) ev = kbEvents[0];
    return ev;
}

kbs_event kb_next(void) {
    kbs_event ev = kb_look_next();
    if (nextKBevent > 0) {
        --nextKBevent;
        memmove(&kbEvents[0], &kbEvents[1], sizeof(kbs_event) * (kNumKBevents - 1));
    }
    return ev;
}

void kb_flush(void) { nextKBevent = 0; }

uchar kb_state(uchar code) { return sshockKeyStates[code] != 0; }

// -------------------------------------------------------------------------
// The mouse queue (as in sdl_events.c)
// -------------------------------------------------------------------------
uchar btn_left = FALSE;
uchar btn_right = FALSE;

errtype mouse_next(ss_mouse_event *res) {
    if (nextMouseEvent <= 0) return ERR_DUNDERFLOW;
    *res = mouseEvents[0];
    --nextMouseEvent;
    memmove(&mouseEvents[0], &mouseEvents[1], sizeof(ss_mouse_event) * (kNumMouseEvents - 1));
    return OK;
}

errtype mouse_flush(void) {
    nextMouseEvent = 0;
    return OK;
}

errtype mouse_get_xy(short *x, short *y) {
    *x = (short)MouseX;
    *y = (short)MouseY;
    return OK;
}

void middleize_mouse(void) {
    MouseX = latestMouseEvent.x = (short)(gScreenWide / 2);
    MouseY = latestMouseEvent.y = (short)(gScreenHigh / 2);
}

void get_mouselook_vel(int *vx, int *vy) {
    if (!rel_mode) {
        *vx = *vy = 0;
    } else {
        *vx = rel_dx + MouseChaosX;
        *vy = rel_dy + MouseChaosY;
        MouseChaosX = MouseChaosY = 0;
    }
    rel_dx = rel_dy = 0;
}

errtype mouse_put_xy(short x, short y) {
    MouseX = x;
    MouseY = y;
    return OK;
}

void set_mouse_chaos(short dx, short dy) {
    MouseChaosX = dx;
    MouseChaosY = dy;
}

void sdl_mouse_init(void) { nextMouseEvent = 0; }
