/*
 * Xnextbsd: X input from WindowServer events, pointer routing and focus.
 *
 * Copyright (c) 2026, Joseph Maloney
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/*
 * One keyboard and one pointer. WindowServer sends input only for our own
 * windows: the pointer while it is over one of them (or while a button
 * pressed in one is held), keys while we are the active app.
 *
 * Keyboard: WindowServer's keycodes are XKB keycodes (evdev code + 8),
 * which are X's keycodes under the evdev rules, so they are passed
 * through. The keymap is WindowServer's own compiled keymap
 * (WS_EVENT_KEYMAP), kept in step when it changes, with its repeat delay
 * and rate; X repeats keys itself, so WindowServer's repeats are dropped.
 * The XKB modifier and layout state follows WS_EVENT_MODIFIERS (latched
 * and locked modifiers, AltGr levels, the layout group), and the keys
 * already down when we become active come with WS_EVENT_KEYS_HELD. A
 * WindowServer without WS_SERVER_XKB sends none of these: the keymap is
 * then compiled from the settings it uses (nbConfig.c), and X tracks the
 * modifiers from the key events alone.
 *
 * Pointer: absolute screen positions, the buttons, and wheel scrolling in
 * 120ths of a detent.
 */

#include <dix-config.h>

#include <sys/mman.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <X11/X.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>

#include "dix/dix_priv.h"
#include "dix/input_priv.h"
#include "dix/inpututils_priv.h"
#include "dix/property_priv.h"
#include "mi/mi_priv.h"
#include "mi/mipointer_priv.h"

#include "scrnintstr.h"
#include "inputstr.h"
#include "mipointer.h"
#include "mi.h"
#include "xkbsrv.h"
#include "xkb/xkbsrv_priv.h"
#include "xserver-properties.h"
#include "exevents.h"
#include "propertyst.h"

#include "nextbsd.h"

#define NB_NBUTTONS 10
#define NB_NAXES 4
#define NB_AXIS_HSCROLL 2
#define NB_AXIS_VSCROLL 3

/* Pixels of smooth scrolling per wheel detent. */
#define NB_SCROLL_PIXELS_PER_DETENT 15.0

static DeviceIntPtr nbPointer;
static DeviceIntPtr nbKeyboard;

/* ---- devices ---- */

static int
nbPointerProc(DeviceIntPtr device, int what)
{
    BYTE map[NB_NBUTTONS + 1];
    Atom btn_labels[NB_NBUTTONS] = { 0 };
    Atom axes_labels[NB_NAXES] = { 0 };
    int i;

    switch (what) {
    case DEVICE_INIT:
        device->public.on = FALSE;

        for (i = 1; i <= NB_NBUTTONS; i++)
            map[i] = i;
        btn_labels[0] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_LEFT);
        btn_labels[1] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_MIDDLE);
        btn_labels[2] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_RIGHT);
        btn_labels[3] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_WHEEL_UP);
        btn_labels[4] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_WHEEL_DOWN);
        btn_labels[5] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_HWHEEL_LEFT);
        btn_labels[6] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_HWHEEL_RIGHT);
        btn_labels[7] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_SIDE);
        btn_labels[8] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_EXTRA);
        btn_labels[9] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_UNKNOWN);

        axes_labels[0] = XIGetKnownProperty(AXIS_LABEL_PROP_ABS_X);
        axes_labels[1] = XIGetKnownProperty(AXIS_LABEL_PROP_ABS_Y);
        axes_labels[NB_AXIS_HSCROLL] =
            XIGetKnownProperty(AXIS_LABEL_PROP_REL_HWHEEL);
        axes_labels[NB_AXIS_VSCROLL] =
            XIGetKnownProperty(AXIS_LABEL_PROP_REL_WHEEL);

        if (!InitPointerDeviceStruct((DevicePtr) device, map, NB_NBUTTONS,
                                     btn_labels, (PtrCtrlProcPtr) NoopDDA,
                                     GetMotionHistorySize(), NB_NAXES,
                                     axes_labels))
            return BadValue;

        InitValuatorAxisStruct(device, 0, axes_labels[0],
                               0, 0xFFFF, 10000, 0, 10000, Absolute);
        InitValuatorAxisStruct(device, 1, axes_labels[1],
                               0, 0xFFFF, 10000, 0, 10000, Absolute);
        InitValuatorAxisStruct(device, NB_AXIS_HSCROLL,
                               axes_labels[NB_AXIS_HSCROLL],
                               NO_AXIS_LIMITS, NO_AXIS_LIMITS, 0, 0, 0,
                               Relative);
        InitValuatorAxisStruct(device, NB_AXIS_VSCROLL,
                               axes_labels[NB_AXIS_VSCROLL],
                               NO_AXIS_LIMITS, NO_AXIS_LIMITS, 0, 0, 0,
                               Relative);
        SetScrollValuator(device, NB_AXIS_HSCROLL, SCROLL_TYPE_HORIZONTAL,
                          1.0, SCROLL_FLAG_NONE);
        SetScrollValuator(device, NB_AXIS_VSCROLL, SCROLL_TYPE_VERTICAL,
                          1.0, SCROLL_FLAG_PREFERRED);
        return Success;

    case DEVICE_ON:
        device->public.on = TRUE;
        if (nbConnection() != NULL)
            SetNotifyFd(ws_connection_fd(nbConnection()), nbInputDispatch,
                        X_NOTIFY_READ, NULL);
        return Success;

    case DEVICE_OFF:
    case DEVICE_CLOSE:
        if (device->public.on && nbConnection() != NULL)
            RemoveNotifyFd(ws_connection_fd(nbConnection()));
        device->public.on = FALSE;
        return Success;
    }
    return BadMatch;
}

static void
nbBell(int percent, DeviceIntPtr device, void *ctrl, int class)
{
    /* TODO: a WindowServer beep (NSBeep) once there is one. */
}

/* WindowServer's repeat delay and interval (0: keys don't repeat). */
static void
nbSetRepeat(DeviceIntPtr device, int delay, int interval)
{
    XkbControlsPtr ctrls = device->key->xkbInfo->desc->ctrls;

    if (interval <= 0) {
        ctrls->enabled_ctrls &= ~XkbRepeatKeysMask;
        return;
    }
    ctrls->enabled_ctrls |= XkbRepeatKeysMask;
    if (delay > 0)
        ctrls->repeat_delay = delay;
    ctrls->repeat_interval = interval;
}

static int
nbKeyboardProc(DeviceIntPtr device, int what)
{
    nbKeyboardConfig cfg;
    XkbRMLVOSet rmlvo;

    switch (what) {
    case DEVICE_INIT:
        device->public.on = FALSE;
        if (nbScreenInfo.keymap != NULL) {
            if (!InitKeyboardDeviceStructFromString(device,
                                                    nbScreenInfo.keymap,
                                                    nbScreenInfo.keymap_len,
                                                    nbBell, NULL))
                return BadValue;
            nbSetRepeat(device, nbScreenInfo.repeat_delay,
                        nbScreenInfo.repeat_interval);
            LogMessageVerb(X_INFO, 0,
                           "Xnextbsd: keyboard: WindowServer's keymap\n");
            return Success;
        }
        /* A WindowServer that doesn't send its keymap: the same settings. */
        nbReadKeyboardConfig(&cfg);
        rmlvo.rules = cfg.rules;
        rmlvo.model = cfg.model;
        rmlvo.layout = cfg.layout;
        rmlvo.variant = cfg.variant[0] ? cfg.variant : NULL;
        rmlvo.options = cfg.options[0] ? cfg.options : NULL;
        LogMessageVerb(X_INFO, 0, "Xnextbsd: keyboard rules %s, model %s, "
                       "layout %s%s%s%s%s\n", rmlvo.rules, rmlvo.model,
                       rmlvo.layout,
                       rmlvo.variant ? ", variant " : "",
                       rmlvo.variant ? rmlvo.variant : "",
                       rmlvo.options ? ", options " : "",
                       rmlvo.options ? rmlvo.options : "");
        if (!InitKeyboardDeviceStruct(device, &rmlvo, nbBell, NULL))
            return BadValue;
        nbSetRepeat(device, cfg.repeat_delay, cfg.repeat_interval);
        return Success;

    case DEVICE_ON:
        device->public.on = TRUE;
        return Success;

    case DEVICE_OFF:
    case DEVICE_CLOSE:
        device->public.on = FALSE;
        return Success;
    }
    return BadMatch;
}

void
InitInput(int argc, char **argv)
{
#if INPUTTHREAD
    /* WindowServer's events are read on the main thread. */
    InputThreadEnable = FALSE;
#endif
    if (AllocDevicePair(serverClient, "Xnextbsd", &nbPointer, &nbKeyboard,
                        nbPointerProc, nbKeyboardProc, FALSE) != Success)
        FatalError("Xnextbsd: can't create the input devices\n");
    mieqInit();
}

void
CloseInput(void)
{
    mieqFini();
}

void
ProcessInputEvents(void)
{
    mieqProcessInputEvents();
}

void
DDXRingBell(int volume, int pitch, int duration)
{
}

/* ---- pointer routing ---- */

/*
 * X decides which window the pointer is in from its own stacking order,
 * but only WindowServer knows which window is really under the pointer:
 * windows of other apps can be in front, and WindowServer raises our
 * windows on click without telling us. So while WindowServer says the
 * pointer is over one of our frames, the search starts from that frame's
 * top-level window; when it says the pointer is over none, X sees the
 * root window. (Xwayland does the same, xwl_xy_to_window.)
 *
 * Override-redirect windows are the exception: they are menus and popups
 * at WindowServer's popup level, above every normal window, and X has them
 * on top too. A menu opened by a press keeps getting the drag while
 * WindowServer still reports the window that was pressed (its implicit
 * grab), so X's own answer is kept whenever it is in one.
 */
static WindowPtr
nbXYToWindow(ScreenPtr pScreen, SpritePtr pSprite, int x, int y)
{
    WindowPtr top = nbFrameWindow(nbScreenInfo.pointer_frame);
    WindowPtr ret, w;

    ret = miXYToWindow(pScreen, pSprite, x, y);
    for (w = ret; w != NULL && w->parent != NULL && w->parent->parent != NULL;
         w = w->parent)
        ;
    if (w != NULL && w->parent != NULL && w->overrideRedirect)
        return ret;

    if (top == NULL || !top->realized) {
        pSprite->spriteTraceGood = 1;
        return pSprite->spriteTrace[0];
    }
    if (x < top->drawable.x - wBorderWidth(top) ||
        y < top->drawable.y - wBorderWidth(top) ||
        x >= top->drawable.x + (int) top->drawable.width + wBorderWidth(top) ||
        y >= top->drawable.y + (int) top->drawable.height + wBorderWidth(top))
        return ret;

    pSprite->spriteTraceGood = 1;
    if (pSprite->spriteTraceSize < 2) {
        WindowPtr *trace = reallocarray(pSprite->spriteTrace, 10,
                                        sizeof(WindowPtr));

        if (trace == NULL)
            return pSprite->spriteTrace[0];
        pSprite->spriteTrace = trace;
        pSprite->spriteTraceSize = 10;
    }
    pSprite->spriteTrace[pSprite->spriteTraceGood++] = top;
    return miSpriteTrace(pSprite, x, y);
}

void
nbInputScreenInit(ScreenPtr pScreen)
{
    nbScreenInfo.XYToWindow = pScreen->XYToWindow;
    pScreen->XYToWindow = nbXYToWindow;
}

static void
nbPointerMotion(double x, double y)
{
    ValuatorMask mask;

    valuator_mask_zero(&mask);
    valuator_mask_set_double(&mask, 0, x);
    valuator_mask_set_double(&mask, 1, y);
    QueuePointerEvents(nbPointer, MotionNotify, 0,
                       POINTER_ABSOLUTE | POINTER_SCREEN, &mask);
}

/* WindowServer's buttons: 0 left, 1 right, 2 middle, then side, extra. */
static int
nbXButton(uint32_t button)
{
    switch (button) {
    case WS_BUTTON_LEFT:
        return 1;
    case WS_BUTTON_MIDDLE:
        return 2;
    case WS_BUTTON_RIGHT:
        return 3;
    default:
        return button + 5 <= NB_NBUTTONS ? (int) button + 5 : 0;
    }
}

static void
nbPointerButton(uint32_t button, Bool pressed)
{
    ValuatorMask mask;
    int b = nbXButton(button);

    if (b == 0)
        return;
    valuator_mask_zero(&mask);
    QueuePointerEvents(nbPointer, pressed ? ButtonPress : ButtonRelease, b,
                       0, &mask);
}

static void
nbPointerScroll(const ws_event_scroll_t *s)
{
    ValuatorMask mask;
    double dx, dy;

    if (s->source == WS_SCROLL_WHEEL) {
        dx = s->v120_x / 120.0;
        dy = s->v120_y / 120.0;
    } else {
        dx = s->dx / NB_SCROLL_PIXELS_PER_DETENT;
        dy = s->dy / NB_SCROLL_PIXELS_PER_DETENT;
    }
    if (dx == 0 && dy == 0)
        return;
    valuator_mask_zero(&mask);
    if (dx != 0)
        valuator_mask_set_double(&mask, NB_AXIS_HSCROLL, dx);
    if (dy != 0)
        valuator_mask_set_double(&mask, NB_AXIS_VSCROLL, dy);
    QueuePointerEvents(nbPointer, MotionNotify, 0, POINTER_RELATIVE, &mask);
}

/*
 * Another app became active: whatever keys and buttons are down in X
 * would stay down, since their releases go to that app. Release them.
 */
static void
nbReleaseAll(void)
{
    ValuatorMask mask;
    int i;

    if (nbKeyboard != NULL && nbKeyboard->key != NULL)
        for (i = nbKeyboard->key->xkbInfo->desc->min_key_code;
             i <= nbKeyboard->key->xkbInfo->desc->max_key_code; i++)
            if (key_is_down(nbKeyboard, i, KEY_PROCESSED))
                QueueKeyboardEvents(nbKeyboard, KeyRelease, i);

    if (nbPointer != NULL && nbPointer->button != NULL) {
        valuator_mask_zero(&mask);
        for (i = 1; i <= NB_NBUTTONS; i++)
            if (button_is_down(nbPointer, i, BUTTON_PROCESSED))
                QueuePointerEvents(nbPointer, ButtonRelease, i, 0, &mask);
    }
}

/* ---- keymap and XKB state ---- */

/*
 * Keep WindowServer's keymap: a sealed memfd holding the keymap text,
 * which is ours to close.
 */
static Bool
nbStoreKeymap(const ws_event_keymap_t *km)
{
    char *map, *copy;
    size_t len;

    if (km->fd < 0)
        return FALSE;
    if (km->format != WS_KEYMAP_XKB_V1 || km->size == 0) {
        close(km->fd);
        return FALSE;
    }
    map = mmap(NULL, km->size, PROT_READ, MAP_PRIVATE, km->fd, 0);
    close(km->fd);
    if (map == MAP_FAILED) {
        ErrorF("Xnextbsd: can't map WindowServer's keymap: %s\n",
               strerror(errno));
        return FALSE;
    }
    len = strnlen(map, km->size);
    copy = malloc(len + 1);
    if (copy != NULL) {
        memcpy(copy, map, len);
        copy[len] = '\0';
    }
    munmap(map, km->size);
    if (copy == NULL)
        return FALSE;
    free(nbScreenInfo.keymap);
    nbScreenInfo.keymap = copy;
    nbScreenInfo.keymap_len = len;
    nbScreenInfo.repeat_delay = (int) km->repeat_delay;
    nbScreenInfo.repeat_interval = (int) km->repeat_interval;
    return TRUE;
}

/*
 * WindowServer sends its keymap right after the welcome. The keyboard is
 * set up from it in InitInput, so wait for it here (a WindowServer with
 * WS_SERVER_XKB always sends it); without it, fall back to the settings.
 */
void
nbInputWaitForKeymap(void)
{
    ws_event_t ev;
    int i, r;

    for (i = 0; i < 64; i++) {
        r = ws_next_event(nbConnection(), &ev, 2000);
        if (r <= 0)
            break;
        if (ev.hdr.type == WS_EVENT_KEYMAP) {
            if (!nbStoreKeymap(&ev.u.keymap))
                break;
            return;
        }
    }
    ErrorF("Xnextbsd: WindowServer sent no keymap; using its keyboard "
           "settings\n");
}

/* The keymap changed (another layout chosen): apply it as Xwayland does. */
static void
nbApplyKeymap(void)
{
    XkbChangesRec changes = { 0 };
    DeviceIntPtr master;
    XkbDescPtr xkb;

    if (nbKeyboard == NULL || nbKeyboard->key == NULL)
        return;
    xkb = XkbCompileKeymapFromString(nbKeyboard, nbScreenInfo.keymap,
                                     nbScreenInfo.keymap_len);
    if (xkb == NULL) {
        ErrorF("Xnextbsd: can't compile WindowServer's new keymap\n");
        return;
    }
    XkbUpdateDescActions(xkb, xkb->min_key_code, XkbNumKeys(xkb), &changes);
    XkbCopyControls(xkb, nbKeyboard->key->xkbInfo->desc);
    XkbDeviceApplyKeymap(nbKeyboard, xkb);
    master = GetMaster(nbKeyboard, MASTER_KEYBOARD);
    if (master != NULL)
        XkbDeviceApplyKeymap(master, xkb);
    XkbFreeKeyboard(xkb, XkbAllComponentsMask, TRUE);
    nbSetRepeat(nbKeyboard, nbScreenInfo.repeat_delay,
                nbScreenInfo.repeat_interval);
}

/*
 * WindowServer's XKB state: depressed, latched and locked modifiers and
 * the layout group, set on our keyboard and its master once the queued
 * key events are processed (Xwayland, keyboard_handle_modifiers).
 */
static void
nbModifiers(const ws_event_modifiers_t *m)
{
    DeviceIntPtr dev, master;
    XkbStateRec old_state, *new_state;
    xkbStateNotify sn;
    CARD16 changed;

    if (nbKeyboard == NULL || nbKeyboard->key == NULL)
        return;
    mieqProcessInputEvents();
    master = GetMaster(nbKeyboard, MASTER_KEYBOARD);
    for (dev = inputInfo.devices; dev != NULL; dev = dev->next) {
        if (dev != nbKeyboard && dev != master)
            continue;
        old_state = dev->key->xkbInfo->state;
        new_state = &dev->key->xkbInfo->state;
        new_state->locked_group = m->group & XkbAllGroupsMask;
        new_state->base_mods = m->depressed & XkbAllModifiersMask;
        new_state->locked_mods = m->locked & XkbAllModifiersMask;
        XkbLatchModifiers(dev, XkbAllModifiersMask,
                          m->latched & XkbAllModifiersMask);
        XkbComputeDerivedState(dev->key->xkbInfo);
        changed = XkbStateChangedFlags(&old_state, new_state);
        if (!changed)
            continue;
        sn.keycode = 0;
        sn.eventType = 0;
        sn.requestMajor = XkbReqCode;
        sn.requestMinor = X_kbLatchLockState;
        sn.changed = changed;
        XkbSendStateNotify(dev, &sn);
    }
}

/* Keys already down when we became active: their releases come to us. */
static void
nbKeysHeld(const ws_event_keys_held_t *held)
{
    uint32_t i, n = min(held->count, WS_MAX_KEYS_HELD);

    if (nbKeyboard == NULL)
        return;
    for (i = 0; i < n; i++)
        if (held->keycodes[i] >= 8 && held->keycodes[i] <= 255 &&
            !key_is_down(nbKeyboard, held->keycodes[i], KEY_PROCESSED))
            QueueKeyboardEvents(nbKeyboard, KeyPress, held->keycodes[i]);
}

/* ---- focus ---- */

/* The client window inside a window manager's frame: the first window,
   breadth first, with WM_STATE, or the top-level window itself. */
static WindowPtr
nbClientWindow(WindowPtr top)
{
    Atom wm_state = dixAddAtom("WM_STATE");
    WindowPtr queue[64];
    int head = 0, tail = 0;
    PropertyPtr prop;
    WindowPtr w, c;

    queue[tail++] = top;
    while (head < tail) {
        w = queue[head++];
        if (dixLookupProperty(&prop, w, wm_state, serverClient,
                              DixReadAccess) == Success)
            return w;
        for (c = w->firstChild; c != NULL && tail < ARRAY_SIZE(queue);
             c = c->nextSib)
            if (c->mapped)
                queue[tail++] = c;
    }
    return top;
}

/* The frame showing a window, or NULL. */
static nbFrame *
nbFrameOfWindow(WindowPtr pWin)
{
    RootlessFrameID wid;

    if (pWin == NULL || pWin->parent == NULL)
        return NULL;
    wid = RootlessFrameForWindow(pWin, FALSE);
    return wid;
}

static WindowPtr
nbFocusWindow(void)
{
    DeviceIntPtr keyboard = inputInfo.keyboard;
    FocusClassPtr focus = keyboard != NULL ? keyboard->focus : NULL;

    if (focus == NULL || focus->win == NoneWin || focus->win == PointerRootWin)
        return NULL;
    return focus->win;
}

/*
 * WindowServer made one of our windows key (the user clicked it, or
 * Command-Tabbed to us). If X's focus is on some other window, move it to
 * the client in that frame. With pointer-root focus (twm's default) the
 * focus already follows the pointer.
 *
 * TODO: through the window manager channel once XLibre.app's window
 * manager runs (apps-xlibre#5).
 */
static void
nbKeyWindowGained(nbFrame *frame)
{
    WindowPtr focus = nbFocusWindow(), top = nbFrameWindow(frame);

    nbScreenInfo.key_frame = frame;
    if (top == NULL || focus == NULL || nbFrameOfWindow(focus) == frame)
        return;
    if (top->overrideRedirect)
        return;
    nbScreenInfo.x_focus_frame = frame;
    SetInputFocus(serverClient, inputInfo.keyboard,
                  nbClientWindow(top)->drawable.id,
                  RevertToPointerRoot, CurrentTime, FALSE);
}

/*
 * The other way: an X client (the window manager, usually) moved the
 * focus to a window in another frame. Tell WindowServer that frame is now
 * our key window. Checked before the server sleeps.
 */
void
nbFocusCheck(void)
{
    nbFrame *frame = nbFrameOfWindow(nbFocusWindow());

    if (frame == nbScreenInfo.x_focus_frame)
        return;
    nbScreenInfo.x_focus_frame = frame;
    if (frame == NULL || frame == nbScreenInfo.key_frame ||
        !frame->ordered_in || (frame->flags & WS_WINDOW_NO_KEY))
        return;
    if (ws_window_make_key(nbConnection(), frame->wid) < 0)
        nbWindowServerLost("make key");
}

/* ---- WindowServer events ---- */

static void
nbPointerEvent(const ws_event_t *ev, Bool entered)
{
    nbFrame *frame = nbFrameFromWID(ev->hdr.window);

    if (frame == NULL)
        return;
    nbScreenInfo.pointer_frame = frame;
    nbPointerMotion(ev->u.pointer.screen_x, ev->u.pointer.screen_y);
}

static void
nbHandleEvent(ws_connection_t *conn, const ws_event_t *ev, void *context)
{
    nbFrame *frame;

    switch (ev->hdr.type) {
    case WS_EVENT_MOUSE_ENTERED:
    case WS_EVENT_MOUSE_MOVED:
    case WS_EVENT_MOUSE_DRAGGED:
        nbPointerEvent(ev, ev->hdr.type == WS_EVENT_MOUSE_ENTERED);
        break;

    case WS_EVENT_MOUSE_EXITED:
        /*
         * The pointer left for another app's window or the desktop. A
         * motion event at the same place lets X see that it is now over
         * the root window (nbXYToWindow), so the X window gets its
         * LeaveNotify.
         */
        frame = nbFrameFromWID(ev->hdr.window);
        if (frame != NULL && frame == nbScreenInfo.pointer_frame) {
            nbScreenInfo.pointer_frame = NULL;
            nbPointerMotion(ev->u.pointer.screen_x, ev->u.pointer.screen_y);
        }
        break;

    case WS_EVENT_MOUSE_DOWN:
    case WS_EVENT_MOUSE_UP:
        nbPointerEvent(ev, FALSE);
        nbPointerButton(ev->u.pointer.button,
                        ev->hdr.type == WS_EVENT_MOUSE_DOWN);
        break;

    case WS_EVENT_SCROLL:
        frame = nbFrameFromWID(ev->hdr.window);
        if (frame != NULL)
            nbScreenInfo.pointer_frame = frame;
        nbPointerScroll(&ev->u.scroll);
        break;

    case WS_EVENT_KEY_DOWN:
    case WS_EVENT_KEY_UP:
        /* X repeats keys itself (XKB autorepeat). */
        if (ev->u.key.flags & WS_KEY_REPEAT)
            break;
        if (ev->u.key.keycode < 8 || ev->u.key.keycode > 255)
            break;
        QueueKeyboardEvents(nbKeyboard,
                            ev->hdr.type == WS_EVENT_KEY_DOWN ?
                            KeyPress : KeyRelease, ev->u.key.keycode);
        break;

    case WS_EVENT_FLAGS_CHANGED:
        /* The coarse WS_MOD_* bits: WS_EVENT_MODIFIERS has the XKB state. */
        break;

    case WS_EVENT_KEYMAP:
        if (nbStoreKeymap(&ev->u.keymap))
            nbApplyKeymap();
        break;

    case WS_EVENT_MODIFIERS:
        nbModifiers(&ev->u.modifiers);
        break;

    case WS_EVENT_KEYS_HELD:
        nbKeysHeld(&ev->u.keys_held);
        break;

    case WS_EVENT_WINDOW_ORDERED:
        frame = nbFrameFromWID(ev->hdr.window);
        if (frame != NULL)
            nbFrameOrdered(frame, &ev->u.ordered);
        break;

    case WS_EVENT_APP_ACTIVATED:
        nbScreenInfo.active = TRUE;
        break;

    case WS_EVENT_APP_DEACTIVATED:
        nbScreenInfo.active = FALSE;
        nbScreenInfo.key_frame = NULL;
        nbReleaseAll();
        break;

    case WS_EVENT_KEY_WINDOW_GAINED:
        frame = nbFrameFromWID(ev->hdr.window);
        if (frame != NULL)
            nbKeyWindowGained(frame);
        break;

    case WS_EVENT_KEY_WINDOW_LOST:
        frame = nbFrameFromWID(ev->hdr.window);
        if (frame != NULL && frame == nbScreenInfo.key_frame)
            nbScreenInfo.key_frame = NULL;
        break;

    case WS_EVENT_SCREEN_CHANGED:
        /* TODO: resize the root window and RandR (apps-xlibre#8). */
        LogMessage(X_WARNING, "Xnextbsd: WindowServer's screen changed "
                   "(%ux%u); not followed yet\n", ev->u.screen.width,
                   ev->u.screen.height);
        break;

    case WS_EVENT_ERROR:
        ErrorF("Xnextbsd: WindowServer error for request %u: %s (%s)\n",
               ev->u.error.request, ev->u.error.message,
               strerror(ev->u.error.code));
        break;

    case WS_EVENT_WINDOW_FRAME:
        /* WindowServer moves only icon windows itself; we have none. */
    case WS_EVENT_BUFFER_RELEASE:
        /* Buffers are destroyed when replaced (single buffered). */
    case WS_EVENT_FRAME_DONE:
    default:
        break;
    }
}

void
nbInputDrain(void)
{
    if (nbConnection() == NULL)
        return;
    if (ws_dispatch(nbConnection(), nbHandleEvent, NULL) < 0) {
        RemoveNotifyFd(ws_connection_fd(nbConnection()));
        nbWindowServerLost("events");
    }
}

void
nbInputDispatch(int fd, int ready, void *data)
{
    nbInputDrain();
}
