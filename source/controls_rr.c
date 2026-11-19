/*
 * controls_rr.c -- PS Vita buttons -> Ridge Racer Slipstream gamepad delegate
 *
 * The game polls RRSSAndroidNwInputControllerDelegate every frame through JNI
 * (CController::Update):
 *   getKeyCode(AKEYCODE_x) -> 0 when pressed (KeyEvent.ACTION_DOWN)
 *   getAxisValue(AXIS_x)   -> float
 * Touch and the accelerometer still go through FalsoNDK as usual.
 */

#include <psp2/ctrl.h>
#include <falso_ndk/FalsoNDK.h>
#include <math.h>
#include <string.h>

int rr_pad_connected = 1;

// Only SELECT is forwarded as a regular Android key event (BACK),
// everything else is read by the game through the controller delegate.
ButtonMapping fndk_button_mapping[] = {
    { SCE_CTRL_SELECT, AKEYCODE_BACK },
};
int fndk_button_mapping_count = 1;

#define DEADZONE 0.15f

// Analog sticks only report real values when analog sampling is enabled for
// the read function in use: FalsoNDK enables it for the Ext2 reads
// (sceCtrlSetSamplingModeExt), so read the pad the same way. Enabling the
// plain mode too keeps both read paths consistent.
static SceCtrlData pad_snapshot(void) {
    static int init;
    if (!init) {
        sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
        sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);
        init = 1;
    }
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    pad.lx = pad.ly = pad.rx = pad.ry = 128;
    sceCtrlPeekBufferPositiveExt2(0, &pad, 1);
    return pad;
}

// Left stick pushed far enough counts as the D-pad, so it navigates menus too.
// In a race the game steers with the analog value first (CControlMapper::
// UpdateFromController ignores the D-pad while the stick is off-centre), and
// up/down are not used for driving.
#define STICK_AS_DPAD 0.6f

// The Ext2 read reports the Vita's L/R shoulders as L1/R1 (not LTRIGGER/
// RTRIGGER); accept both so L/R work whatever the read function / pad.
#define PAD_L (SCE_CTRL_L1 | SCE_CTRL_LTRIGGER)
#define PAD_R (SCE_CTRL_R1 | SCE_CTRL_RTRIGGER)

int rr_key_down(int code) {
    SceCtrlData pad = pad_snapshot();
    uint32_t b = pad.buttons;
    float lx = ((float)pad.lx - 128.0f) / 127.0f, ly = ((float)pad.ly - 128.0f) / 127.0f;
    switch (code) {
        case 19:  return (b & SCE_CTRL_UP) != 0 || ly < -STICK_AS_DPAD;     // DPAD_UP
        case 20:  return (b & SCE_CTRL_DOWN) != 0 || ly > STICK_AS_DPAD;    // DPAD_DOWN
        case 21:  return (b & SCE_CTRL_LEFT) != 0 || lx < -STICK_AS_DPAD;   // DPAD_LEFT
        case 22:  return (b & SCE_CTRL_RIGHT) != 0 || lx > STICK_AS_DPAD;   // DPAD_RIGHT
        case 96:  return (b & SCE_CTRL_CROSS) != 0;     // BUTTON_A
        case 97:  return (b & SCE_CTRL_CIRCLE) != 0;    // BUTTON_B
        case 99:  return (b & SCE_CTRL_SQUARE) != 0;    // BUTTON_X
        case 100: return (b & SCE_CTRL_TRIANGLE) != 0;  // BUTTON_Y
        case 102: return 0;                 // BUTTON_L1: L brakes (AXIS_LTRIGGER) instead
        case 103: return (b & PAD_R) != 0;  // BUTTON_R1 (nitro)
        case 108: return (b & SCE_CTRL_START) != 0;     // BUTTON_START
        default:  return 0;
    }
}

static float stick(uint8_t v) {
    float f = ((float)v - 128.0f) / 127.0f;
    if (f > 1.0f) f = 1.0f;
    if (f < -1.0f) f = -1.0f;
    if (fabsf(f) < DEADZONE) return 0.0f;
    return f;
}

float rr_axis_value(int axis) {
    SceCtrlData pad = pad_snapshot();
    switch (axis) {
        case 0:  return stick(pad.lx);   // AXIS_X
        case 1:  return stick(pad.ly);   // AXIS_Y
        case 11: return stick(pad.rx);   // AXIS_Z  (right stick X)
        case 14: return stick(pad.ry);   // AXIS_RZ (right stick Y)
        // L = brake: the game brakes with the analog L trigger (AXIS_LTRIGGER).
        // R = nitro (BUTTON_R1, code 103), so the R trigger axis stays at 0.
        case 17: return (pad.buttons & PAD_L) ? 1.0f : 0.0f;
        case 18: return 0.0f;
        default: return 0.0f;
    }
}


/*
 * Menu navigation. The game's MenuController only follows the links each
 * menu item defines; rolling menus (wheels) rotate with the right stick only,
 * unless a screen turns on the game's "basic configuration". So on a wheel,
 * a D-pad direction it has no link for is sent as the matching right-stick
 * transition (see mc_do_transition). The profiler build also logs every
 * transition (with on-screen positions) to menu.txt.
 *
 * MenuController::DoTransition(t): page = mc+16; page->vtable[2](page, t)
 * slot links: vector<MenuControllerSlotBase*> at slot + 24 + t*12 (t 0..3:
 * left, right, up, down)
 */
#include <so_util/so_util.h>

extern so_module so_mod;
static void *(*mc_GetCurrent)(void *mc);
static void (*frame_to_screen)(float out[2], void *frame, float x, float y);
static void *roller_vtable;   // MenuControllerSlotRoller

// screen position of a menu item's frame origin (0 if unknown)
static int slot_screen_pos(void *slot, float *x, float *y) {
    if (!slot || !frame_to_screen) return 0;
    void *frame = *(void **)((uint8_t *)slot + 12);   // MenuControllerSlotBase::Frame()
    if (!frame) return 0;
    float v[2] = { 0.0f, 0.0f };
    frame_to_screen(v, frame, 0.0f, 0.0f);
    *x = v[0]; *y = v[1];
    return 1;
}

static void *first_link(void *slot, int t) {
    uint8_t *v = (uint8_t *)slot + 24 + t * 12;
    void **b = *(void ***)v, **e = *(void ***)(v + 4);
    return (b && b != e) ? b[0] : NULL;
}

#ifdef PERF_PROFILE
void perf_menu_log(void *mc, int t_in, int t_used, void *before, void *after, int links);
void perf_menu_note(const char *s);
#include <stdio.h>
#endif

static int slot_links(void *slot, int t) {
    if (!slot || t < 0 || t > 3) return -1;
    uint8_t *v = (uint8_t *)slot + 24 + t * 12;
    return (int)((*(uint8_t **)(v + 4) - *(uint8_t **)v) / 4);
}

static int mc_do_transition(uint8_t *mc, int t) {
    void *page = *(void **)(mc + 16);
    if (!page) return 0;
    int t_in = t;
    void *cur = mc_GetCurrent(mc);
    // Rolling menus (the main menu wheel, car / track pickers) are a single
    // menu item that only rotates with the right stick (transitions 5-8) unless
    // the screen enables the game's "basic configuration". When the D-pad is
    // pressed on a wheel in a direction it has no link for, send the right-stick
    // transition the wheel rotates with. Everything else is unchanged.
    if (cur && roller_vtable && *(void **)cur == roller_vtable && t >= 0 && t <= 3 &&
        !((uint8_t *)mc)[22] && slot_links(cur, t) == 0) {
        int horizontal = *(int *)((uint8_t *)cur + 104) == 1;
        if (horizontal ? (t == 0 || t == 1) : (t == 2 || t == 3))
            t += 5;   // 0/1 -> 5/6 (rs left/right), 2/3 -> 7/8 (rs up/down)
    }
#ifdef PERF_PROFILE
    {
        char b[200]; float x0 = 0, y0 = 0, x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        int p0 = slot_screen_pos(cur, &x0, &y0);
        int p1 = cur && first_link(cur, 0) ? slot_screen_pos(first_link(cur, 0), &x1, &y1) : 0;
        int p2 = cur && first_link(cur, 1) ? slot_screen_pos(first_link(cur, 1), &x2, &y2) : 0;
        snprintf(b, sizeof(b), "   screen: item %s(%.0f,%.0f) left-link %s(%.0f,%.0f) right-link %s(%.0f,%.0f)%s\n",
                 p0 ? "" : "?", x0, y0, p1 ? "" : "-", x1, y1, p2 ? "" : "-", x2, y2,
                 t != t_in ? "  -> wheel: sent as right-stick transition" : "");
        perf_menu_note(b);
    }
#endif
    int r = ((int (*)(void *, int))(*(void ***)page)[2])(page, t);
#ifdef PERF_PROFILE
    perf_menu_log(mc, t_in, t, cur, mc_GetCurrent(mc),
                  slot_links(cur, 0) | slot_links(cur, 1) << 4 | slot_links(cur, 2) << 8 | slot_links(cur, 3) << 12);
#else
    (void)t_in;
#endif
    return r;
}

/*
 * Game bug guard (crash seen on HW 2026-11-18): CMenuControll::SwitchMenu ->
 * MenuController::SetCurrent(new) un-focuses the previous slot first
 * (vtable[4] -> MenuControllerSlotBase::SetFocused(false) -> frame->GetColor),
 * but that slot's frame can already be deleted when the old screen was torn
 * down (use-after-free -> jump through a garbage vtable). On Android the
 * gamepad menu controller is normally inactive, so this path rarely runs.
 * If the old slot's frame no longer has a vtable inside the game module, the
 * stale pointer is dropped before calling the original.
 */
static void (*mc_set_current_orig)(uint8_t *mc, void *slot);
static uintptr_t mod_lo, mod_hi;

static int live_object(const void *o) {
    if (!o || ((uintptr_t)o & 3)) return 0;
    uintptr_t vp = *(const uintptr_t *)o;
    return !(vp & 3) && vp >= mod_lo && vp < mod_hi;
}

static void mc_set_current(uint8_t *mc, void *slot) {
    uint8_t *old = *(uint8_t **)(mc + 16);
    if (old) {
        void *frame = *(void **)(old + 16);           // colour-override frame
        if (!frame) frame = *(void **)(old + 12);      // MenuControllerSlotBase::Frame()
        if (!live_object(old) || (frame && !live_object(frame)))
            *(void **)(mc + 16) = NULL;
    }
    mc_set_current_orig(mc, slot);
}

void *fp_make_tramp(uintptr_t a);

static void install_set_current_guard(void) {
    uintptr_t a = so_symbol(&so_mod, "_ZN14MenuController10SetCurrentEP22MenuControllerSlotBase");
    const uint32_t *f = (const uint32_t *)a;
    // 2.5.4: push {r3,r4,r5,r6,fp,lr}; subs r6, r1, #0; add fp, sp, #20
    if (!a || f[0] != 0xE92D4878 || f[1] != 0xE2516000 || f[2] != 0xE28DB014) return;
    mod_lo = so_mod.text_base;
    mod_hi = so_mod.text_base + so_mod.text_size;
    for (int i = 0; i < MAX_DATA_SEG; i++)
        if (so_mod.data_size[i] && so_mod.data_base[i] + so_mod.data_size[i] > mod_hi)
            mod_hi = so_mod.data_base[i] + so_mod.data_size[i];
    if (!(*(void **)&mc_set_current_orig = fp_make_tramp(a))) return;
    hook_addr(a, (uintptr_t)&mc_set_current);
}

void controls_menu_patch(void) {
    install_set_current_guard();
    frame_to_screen = (void *)so_symbol(&so_mod, "_ZN8CFrame2D17TransformToScreenEff");
    uintptr_t vt = so_symbol(&so_mod, "_ZTV24MenuControllerSlotRoller");
    if (vt) roller_vtable = (void *)(vt + 8);   // object vptr = vtable + 2 words
    uintptr_t a = so_symbol(&so_mod, "_ZN14MenuController12DoTransitionEN22MenuControllerSlotBase20ControllerTransitionE");
    mc_GetCurrent = (void *)so_symbol(&so_mod, "_ZN14MenuController10GetCurrentEv");
    const uint32_t *f = (const uint32_t *)a;
    // 2.5.4: push {fp,lr}; add fp,sp,#4; ldr r0,[r0,#16]; cmp r0,#0; popeq; ldr r3,[r0]; ldr r3,[r3,#8]; blx r3
    if (!a || !mc_GetCurrent || f[0] != 0xE92D4800 || f[2] != 0xE5900010 || f[6] != 0xE5933008)
        return;
    hook_addr(a, (uintptr_t)&mc_do_transition);
}
