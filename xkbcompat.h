/* See LICENSE file for copyright and license details.

 * X11 compatibility layer.
 *
 * vxrwm keeps dwm's configuration files (config.h, modules.h) untouched, so
 * everything the X11 build referenced through Xlib/Xft is mapped onto its
 * xkbcommon / linux-input / river equivalent here:
 *
 *   XK_*      -> XKB_KEY_*   (xkbcommon/xkbcommon-keysyms.h)
 *   KeySym    -> xkb_keysym_t
 *   ButtonN   -> Linux input event code (linux/input-event-codes.h)
 *   *Mask     -> river_seat_v1.modifiers bits (which are the X11 bit values)
 */

#pragma once

#include <stdbool.h>

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

/* --- keysyms ---------------------------------------------------------- */

typedef xkb_keysym_t KeySym;

#define XK_0             XKB_KEY_0
#define XK_1             XKB_KEY_1
#define XK_2             XKB_KEY_2
#define XK_3             XKB_KEY_3
#define XK_4             XKB_KEY_4
#define XK_5             XKB_KEY_5
#define XK_6             XKB_KEY_6
#define XK_7             XKB_KEY_7
#define XK_8             XKB_KEY_8
#define XK_9             XKB_KEY_9
#define XK_b             XKB_KEY_b
#define XK_c             XKB_KEY_c
#define XK_comma         XKB_KEY_comma
#define XK_d             XKB_KEY_d
#define XK_Down          XKB_KEY_Down
#define XK_equal         XKB_KEY_equal
#define XK_f             XKB_KEY_f
#define XK_F5            XKB_KEY_F5
#define XK_h             XKB_KEY_h
#define XK_i             XKB_KEY_i
#define XK_j             XKB_KEY_j
#define XK_k             XKB_KEY_k
#define XK_l             XKB_KEY_l
#define XK_Left          XKB_KEY_Left
#define XK_m             XKB_KEY_m
#define XK_minus         XKB_KEY_minus
#define XK_Num_Lock      XKB_KEY_Num_Lock
#define XK_p             XKB_KEY_p
#define XK_period        XKB_KEY_period
#define XK_q             XKB_KEY_q
#define XK_r             XKB_KEY_r
#define XK_Return        XKB_KEY_Return
#define XK_Right         XKB_KEY_Right
#define XK_space         XKB_KEY_space
#define XK_t             XKB_KEY_t
#define XK_Tab           XKB_KEY_Tab
#define XK_Up            XKB_KEY_Up
#define XK_z             XKB_KEY_z

/* --- pointer buttons ---------------------------------------------------
 * River's river_pointer_binding_v1 takes a Linux input event code.  There is
 * no scroll wheel code, so Button4/Button5 are private sentinels (bit 30
 * flags "scroll") which never reach get_pointer_binding().  Scroll bindings
 * are dispatched from the window manager's own shell surfaces instead.
 */

#define Button1          BTN_LEFT
#define Button2          BTN_MIDDLE
#define Button3          BTN_RIGHT
#define Button4          0x40000004u
#define Button5          0x40000005u
#define Button6          0x40000006u
#define Button7          0x40000007u
#define IS_SCROLL_BTN(b) (((unsigned int)(b) & 0x40000000u) != 0)

/* --- keyboard modifiers ------------------------------------------------
 * river_seat_v1.modifiers uses exactly the X11 mask values.  LockMask and
 * Mod2Mask (numlock) are not part of river's binding enum; the compositor
 * ignores locked modifiers when matching, and CLEANMASK drops them too.
 */

#define ShiftMask        (1 << 0)
#define LockMask         (1 << 1)
#define ControlMask      (1 << 2)
#define Mod1Mask         (1 << 3)
#define Mod2Mask         (1 << 4)
#define Mod3Mask         (1 << 5)
#define Mod4Mask         (1 << 6)
#define Mod5Mask         (1 << 7)

/* --- Xlib shims used by config.h / modules ---------------------------- */

#ifndef True
#define True  1
#define False 0
#endif
#define CurrentTime 0
#define AnyButton 0
#define AnyModifier 0

/* X11 cursor font indices, translated to wp_cursor_shape_device_v1 shapes
 * by drw_cur_create(). */
enum {
	XC_left_ptr = 1,
	XC_sizing,
	XC_fleur,
	XC_top_left_corner,
	XC_top_right_corner,
	XC_bottom_left_corner,
	XC_bottom_right_corner,
	XC_top_side,
	XC_bottom_side,
	XC_left_side,
	XC_right_side
};
