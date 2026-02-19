// Adapted from hid_controller example from TinyUSB for PicoGUS
/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2021, Ha Thach (tinyusb.org)
 * Copyright (c) 2023, Ian Scott
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include "tusb.h"
#include "host/hcd.h"
#ifdef USB_JOYSTICK
#include "xinput_host.h"
#endif
#ifdef USB_MOUSE
#include "sermouse.h"
#endif

// Counts how many USB devices of ANY type are currently mounted.
// Tracks how many USB devices are currently mounted.
// Driven by tuh_mount_cb / tuh_umount_cb below, which TinyUSB fires for every
// device class (HID, MSC, XInput, CDC, …) after successful enumeration.
// Accessed only from TinyUSB task context (core 1), so plain int is safe.
static int usb_mounted_count = 0;

// Device-level callbacks — fired by TinyUSB for every device type.
void tuh_mount_cb(uint8_t dev_addr) {
    usb_mounted_count++;
    uint16_t vid, pid;
    tuh_vid_pid_get(dev_addr, &vid, &pid);
    printf("USB dev %d mounted: VID=0x%04x PID=0x%04x\r\n", dev_addr, vid, pid);
}

void tuh_umount_cb(uint8_t dev_addr) {
    (void) dev_addr;
    if (usb_mounted_count > 0) usb_mounted_count--;
}

// Called every iteration of any play_*() main loop that has USB_STACK enabled.
// Tracks physical connect/disconnect transitions and logs them.
//
// NOTE: The previous implementation attempted tuh_deinit()/tuh_init() when
// enumeration appeared to stall.  On RP2040, hcd_deinit() issues a full
// reset_block(RESETS_RESET_USBCTRL_BITS) which resets the single shared USB
// hardware peripheral.  Calling this while TinyUSB may be mid-transaction
// corrupts internal stack state and can cause a hard fault / deadlock that
// freezes the firmware entirely.  TinyUSB's own state machine handles
// re-enumeration correctly as long as tuh_task() is called regularly, so the
// reinit watchdog has been removed.
void usb_hotplug_task(void) {
    static bool prev_phys = false;

    bool phys = hcd_port_connect_status(BOARD_TUH_RHPORT);
    if (phys != prev_phys) {
        prev_phys = phys;
        printf("USB physical %s\r\n", phys ? "connected" : "disconnected");
    }
}

#define MAX_REPORT 4
static struct
{
  uint8_t report_count;
  tuh_hid_report_info_t report_info[MAX_REPORT];
} hid_info[CFG_TUH_HID];

#ifdef USB_JOYSTICK
#include "joy.h"
joystate_struct_t joystate_struct;

/* Last HID button index pressed on any connected joystick/gamepad.
 * 0xFF = no button pressed since last read.
 * Written by ghid_process_report(); read and cleared by picogus.cpp CMD_JOY_LAST_BTN. */
volatile uint8_t joy_last_btn = 0xFF;

// ============================================================
// Virtual Gamepad (VGP) — controller-agnostic intermediate
// representation used by all driver paths before writing to
// joystate_struct.  All drivers normalize to vgp_state_t, then
// vgp_to_joystate() applies the (currently hardcoded) default
// Gravis-compatible mapping.  When the configurable INI profile
// is added (see BUTTON_MAPPING_PLAN.md) only vgp_to_joystate()
// needs to change.
// ============================================================
typedef enum {
    VGP_BTN_SOUTH  = 0,  // A (Xbox), Cross (PS), B (Nintendo)
    VGP_BTN_EAST   = 1,  // B (Xbox), Circle (PS), A (Nintendo)
    VGP_BTN_WEST   = 2,  // X (Xbox), Square (PS), Y (Nintendo)
    VGP_BTN_NORTH  = 3,  // Y (Xbox), Triangle (PS), X (Nintendo)
    VGP_BTN_LB     = 4,  // Left shoulder
    VGP_BTN_RB     = 5,  // Right shoulder
    VGP_BTN_LT     = 6,  // Left trigger (digital)
    VGP_BTN_RT     = 7,  // Right trigger (digital)
    VGP_BTN_SELECT = 8,  // Back / Share / Select / Minus
    VGP_BTN_START  = 9,  // Start / Options / Plus
    VGP_BTN_L3     = 10, // Left stick click
    VGP_BTN_R3     = 11, // Right stick click
    VGP_BTN_DUP    = 12,
    VGP_BTN_DDOWN  = 13,
    VGP_BTN_DLEFT  = 14,
    VGP_BTN_DRIGHT = 15,
} vgp_button_t;

typedef enum {
    VGP_AXIS_LEFT_X  = 0, // All normalized 0-255, 127 = center
    VGP_AXIS_LEFT_Y  = 1,
    VGP_AXIS_RIGHT_X = 2,
    VGP_AXIS_RIGHT_Y = 3,
    VGP_AXIS_LT      = 4, // Analog trigger (0=released, 255=full)
    VGP_AXIS_RT      = 5,
    VGP_AXIS_COUNT   = 6
} vgp_axis_t;

typedef struct {
    uint16_t buttons;              // Bit N = (1u << vgp_button_t value)
    uint8_t  axes[VGP_AXIS_COUNT];
} vgp_state_t;

// ============================================================
// Joystick profile constants — defined in joy.h:
//   JOY_PROFILE_GAMEPAD          (0, default) — standard gamepad axis routing
//   JOY_PROFILE_THROTTLE         (1) — HOTAS analog throttle
//   JOY_PROFILE_CH_FLIGHTSTICK   (2) — CH FlightStick Pro 4-bit combo encoding
//   JOY_PROFILE_THRUSTMASTER_FCS (3) — Thrustmaster FCS hat-as-analog
//
// Button mapping is handled separately via btn1-4 / btn1_alt-4_alt fields.
// Runtime config struct — also defined in joy.h (joy_config_t).
// ============================================================

// Compile-time defaults — set in joy.h user config block.
// These become factory reset values once flash settings support is added.
#ifndef JOY_PROFILE_DEFAULT
#define JOY_PROFILE_DEFAULT         JOY_PROFILE_GAMEPAD
#endif
#ifndef JOY_JOY1_DEADZONE_DEFAULT
#define JOY_JOY1_DEADZONE_DEFAULT   0
#endif
#ifndef JOY_JOY2_DEADZONE_DEFAULT
#define JOY_JOY2_DEADZONE_DEFAULT   0
#endif

joy_config_t joy_config = {
    .profile              = JOY_PROFILE_DEFAULT,
    .joy1_deadzone        = JOY_JOY1_DEADZONE_DEFAULT,
    .joy2_deadzone        = JOY_JOY2_DEADZONE_DEFAULT,
    .swap_throttle_rudder = JOY_SWAP_THROTTLE_DEFAULT,
    .btn1                 = JOY_BTN1_DEFAULT,
    .btn2                 = JOY_BTN2_DEFAULT,
    .btn3                 = JOY_BTN3_DEFAULT,
    .btn4                 = JOY_BTN4_DEFAULT,
    .joy1_btn_layout      = JOY_JOY1_BTN_LAYOUT_DEFAULT,
    .joy2_btn_layout      = JOY_JOY2_BTN_LAYOUT_DEFAULT,
    .invert_joy1_y        = JOY_INVERT_JOY1_Y_DEFAULT,
    .invert_joy2_y        = JOY_INVERT_JOY2_Y_DEFAULT,
};

// -----------------------------------------------------------------------
// Dual-joystick device slot tracking
//
// The DOS gameport exposes two independent joysticks on one port:
//   Slot 0 (joy1): axes joy1_x/y, buttons bits 4-5
//   Slot 1 (joy2): axes joy2_x/y, buttons bits 6-7
//
// The first USB device that mounts as a valid joystick/gamepad takes slot 0.
// The second takes slot 1.  Disconnecting a device frees its slot; the other
// device stays in place.  With only one device connected, all existing
// single-device profiles (CH, FCS, Throttle, etc.) work exactly as before.
// With two devices, both use simple 2-axis + 2-button mapping.
// -----------------------------------------------------------------------
#define JOY_SLOT_NONE 0xFF
static uint8_t joy_slot_dev[2] = {JOY_SLOT_NONE, JOY_SLOT_NONE}; // dev_addr per slot

// Return which slot (0 or 1) owns dev_addr, or JOY_SLOT_NONE if unassigned.
static inline uint8_t joy_get_slot(uint8_t dev_addr) {
    if (joy_slot_dev[0] == dev_addr) return 0;
    if (joy_slot_dev[1] == dev_addr) return 1;
    return JOY_SLOT_NONE;
}

// Assign dev_addr to the first free slot.  Returns the assigned slot, or
// JOY_SLOT_NONE if both slots are already occupied.
// Safe to call multiple times for the same dev_addr — returns the existing
// slot if already assigned rather than double-assigning.
static inline uint8_t joy_assign_slot(uint8_t dev_addr) {
    // Already assigned? Return existing slot.
    uint8_t existing = joy_get_slot(dev_addr);
    if (existing != JOY_SLOT_NONE) return existing;
    // Find first free slot.
    for (uint8_t s = 0; s < 2; s++) {
        if (joy_slot_dev[s] == JOY_SLOT_NONE) {
            joy_slot_dev[s] = dev_addr;
            printf("Joy slot %u assigned to dev_addr %u\r\n", s, dev_addr);
            return s;
        }
    }
    printf("Joy slots full, dev_addr %u ignored\r\n", dev_addr);
    return JOY_SLOT_NONE;
}

// Free the slot held by dev_addr (called on unmount).
// If slot 0 is freed and slot 1 is occupied, promote slot 1 → slot 0 so the
// surviving device gets full single-device profile support immediately.
static inline void joy_free_slot(uint8_t dev_addr) {
    for (uint8_t s = 0; s < 2; s++) {
        if (joy_slot_dev[s] == dev_addr) {
            printf("Joy slot %u freed (dev_addr %u)\r\n", s, dev_addr);
            joy_slot_dev[s] = JOY_SLOT_NONE;
            // Promote slot 1 → slot 0 if slot 0 just became free
            if (s == 0 && joy_slot_dev[1] != JOY_SLOT_NONE) {
                joy_slot_dev[0] = joy_slot_dev[1];
                joy_slot_dev[1] = JOY_SLOT_NONE;
                printf("Joy slot 1 promoted to slot 0 (dev_addr %u)\r\n", joy_slot_dev[0]);
            }
            return;
        }
    }
}

// True when two devices are currently occupying both slots.
static inline bool joy_dual_active(void) {
    return joy_slot_dev[0] != JOY_SLOT_NONE && joy_slot_dev[1] != JOY_SLOT_NONE;
}

// Apply a center dead zone to a normalized [0,255] axis value.
// Values within 'dz' of center are snapped to 127; outside values are
// rescaled so the full 0-255 range is still reachable.
static inline uint8_t apply_deadzone(uint8_t val, uint8_t dz) {
    if (dz == 0) return val;
    int16_t v = (int16_t)val - 127;
    if (v < 0 ? -v <= dz : v <= dz) return 127;
    int16_t sign = (v > 0) ? 1 : -1;
    int16_t abs_v = v * sign;
    int16_t scaled = (int16_t)(((int32_t)(abs_v - dz) * 127) / (127 - dz));
    return (uint8_t)(127 + sign * scaled);
}

// Map VGP state to joystate_struct.
// Axis and button_mask encoding varies by joy_config.profile — see joy.h for details.
// When dual joysticks are active (joy_dual_active()), the complex single-device
// profiles are bypassed and slot 0 uses simple 2-axis + 2-button mapping so that
// joy2_x/y and bits 6-7 remain available for the second device.
static void vgp_to_joystate(const vgp_state_t* vgp) {
    uint16_t btns = vgp->buttons;

    // Left stick X/Y — common to all profiles
    joystate_struct.joy1_x = apply_deadzone(vgp->axes[VGP_AXIS_LEFT_X], joy_config.joy1_deadzone);
    uint8_t raw_y = vgp->axes[VGP_AXIS_LEFT_Y];
    if (joy_config.invert_joy1_y) raw_y = 255 - raw_y;
    joystate_struct.joy1_y = apply_deadzone(raw_y, joy_config.joy1_deadzone);

    // In dual mode, use simple mapping for slot 0 so joy2 is free for device 2.
    if (joy_dual_active()) {
        // D-pad overrides left stick
        if (btns & ((1u << VGP_BTN_DUP)   | (1u << VGP_BTN_DDOWN) |
                    (1u << VGP_BTN_DLEFT) | (1u << VGP_BTN_DRIGHT))) {
            joystate_struct.joy1_x = (btns & (1u << VGP_BTN_DRIGHT)) ? 255 :
                                      (btns & (1u << VGP_BTN_DLEFT))  ? 0   : 127;
            joystate_struct.joy1_y = (btns & (1u << VGP_BTN_DDOWN))  ? 255 :
                                      (btns & (1u << VGP_BTN_DUP))    ? 0   : 127;
        }
        // Bits 4-5 = joy1 buttons (active-LOW); bits 6-7 left for device 2
        // btn1/btn2 drive joy1 buttons in dual mode.
        joystate_struct.button_mask =
            (joystate_struct.button_mask & 0xC0u) |  // preserve joy2 bits
            (!(btns & (1u << joy_config.btn1)) << 4) |
            (!(btns & (1u << joy_config.btn2)) << 5);
        return;
    }

    // Single-device path — full profile logic below.

    // D-pad overrides left stick when any direction is pressed.
    // Skip for CH and FCS profiles — on those, the hat feeds button_mask /
    // joy2_y encoding below and must NOT fight the analog stick axes.
    if (joy_config.profile != JOY_PROFILE_CH_FLIGHTSTICK &&
        joy_config.profile != JOY_PROFILE_THRUSTMASTER_FCS) {
        if (btns & ((1u << VGP_BTN_DUP)   | (1u << VGP_BTN_DDOWN) |
                    (1u << VGP_BTN_DLEFT) | (1u << VGP_BTN_DRIGHT))) {
            joystate_struct.joy1_x = (btns & (1u << VGP_BTN_DRIGHT)) ? 255 :
                                      (btns & (1u << VGP_BTN_DLEFT))  ? 0   : 127;
            joystate_struct.joy1_y = (btns & (1u << VGP_BTN_DDOWN))  ? 255 :
                                      (btns & (1u << VGP_BTN_DUP))    ? 0   : 127;
        }
    }

    // Analog triggers override joy1_y (throttle/brake for driving games)
    uint8_t lt = vgp->axes[VGP_AXIS_LT];
    uint8_t rt = vgp->axes[VGP_AXIS_RT];
    if (lt) {
        joystate_struct.joy1_y = 127u + (lt >> 1);
    } else if (rt) {
        joystate_struct.joy1_y = 127u - (rt >> 1);
    }

    // ---------------------------------------------------------------
    // joy2 axes and button_mask — profile-specific
    // ---------------------------------------------------------------

    if (joy_config.profile == JOY_PROFILE_CH_FLIGHTSTICK) {
        // CH FlightStick Pro protocol (6-button + 2-hat variant, as emulated by DOSBox):
        //   joy2_x = twist/rudder  (axis 2, right stick X)
        //   joy2_y = throttle wheel (axis 3, right stick Y)
        //
        // Each nibble of the 16-bit state word drives one gameport button line.
        // Any non-zero nibble = that line is pressed (active-LOW in button_mask).
        // We use hat_magic[0] for the primary hat; hat takes priority over buttons.
        joystate_struct.joy2_x = vgp->axes[VGP_AXIS_RIGHT_X]; // twist/rudder
        joystate_struct.joy2_y = vgp->axes[VGP_AXIS_RIGHT_Y]; // throttle wheel

        bool hat_up    = (btns & (1u << VGP_BTN_DUP))    != 0;
        bool hat_down  = (btns & (1u << VGP_BTN_DDOWN))  != 0;
        bool hat_left  = (btns & (1u << VGP_BTN_DLEFT))  != 0;
        bool hat_right = (btns & (1u << VGP_BTN_DRIGHT)) != 0;
        bool b1 = (btns & (1u << VGP_BTN_SOUTH)) != 0;
        bool b2 = (btns & (1u << VGP_BTN_EAST))  != 0;
        bool b3 = (btns & (1u << VGP_BTN_WEST))  != 0;
        bool b4 = (btns & (1u << VGP_BTN_NORTH)) != 0;
        bool b5 = (btns & (1u << VGP_BTN_LB))    != 0;
        bool b6 = (btns & (1u << VGP_BTN_RB))    != 0;

        // Button/hat encoding derived from the CH FlightStick Pro hardware protocol.
        // Reference: DOSBox-X CCHBindGroup (GPL v2, https://github.com/joncampbell123/dosbox-x)
        //   button_magic[6] = {0x02, 0x04, 0x10, 0x100, 0x20, 0x200}
        //   hat_magic[0]    = {clear:0x8888, U:0x8000, R:0x800, D:0x80, L:0x08}
        // Build 16-bit state word from hat_magic[0] and button_magic tables,
        // then decode to 4 active-LOW button bits (bits 4-7 of button_mask).
        // Hat takes priority; within buttons, lower-numbered wins.
        uint16_t state = 0;
        // hat_magic[0]: clear=0x8888, U=0x8000, R=0x800, D=0x80, L=0x08
        if (hat_up)    state |= 0x8000u;
        if (hat_right) state |= 0x0800u;
        if (hat_down)  state |= 0x0080u;
        if (hat_left)  state |= 0x0008u;
        // button_magic: b1=0x02, b2=0x04, b3=0x10, b4=0x100, b5=0x20, b6=0x200
        if (!hat_up && !hat_down && !hat_left && !hat_right) {
            if (b1) state |= 0x0002u;
            if (b2) state |= 0x0004u;
            if (b3) state |= 0x0010u;
            if (b4) state |= 0x0100u;
            if (b5) state |= 0x0020u;
            if (b6) state |= 0x0200u;
        }
        // Decode state to 4 button lines (active-LOW in bits 4-7):
        // Each nibble of state drives one button line; any non-zero nibble = pressed.
        bool line1 = (state & 0x000Fu) != 0; // bit4
        bool line2 = (state & 0x00F0u) != 0; // bit5
        bool line3 = (state & 0x0F00u) != 0; // bit6
        bool line4 = (state & 0xF000u) != 0; // bit7
        joystate_struct.button_mask =
            (!line1 << 4) | (!line2 << 5) | (!line3 << 6) | (!line4 << 7);

        // --- CH DEBUG ---
        // Uncomment to see hat/button encoding on serial terminal
//#define GHID_DEBUG_CH
#ifdef GHID_DEBUG_CH
        {
            static uint8_t _prev = 0xFF;
            if (joystate_struct.button_mask != _prev) {
                _prev = joystate_struct.button_mask;
                printf("CH mask=0x%02x state=0x%04x hat=%s%s%s%s btn=%s%s%s%s%s%s\r\n",
                    joystate_struct.button_mask, (unsigned)state,
                    hat_up?"U":"", hat_down?"D":"", hat_left?"L":"", hat_right?"R":"",
                    b1?"1":"", b2?"2":"", b3?"3":"", b4?"4":"", b5?"5":"", b6?"6":"");
            }
        }
#endif

    } else if (joy_config.profile == JOY_PROFILE_THRUSTMASTER_FCS) {
        // Thrustmaster FCS protocol:
        // joy2_x = rudder (right stick X)
        // joy2_y = hat direction encoded as analog value:
        //   center/released = 255, N = 0, NE = 32, E = 64, SE = 96,
        //   S = 128, SW = 160, W = 192, NW = 224
        joystate_struct.joy2_x = vgp->axes[VGP_AXIS_RIGHT_X];

        bool hat_up    = (btns & (1u << VGP_BTN_DUP))    != 0;
        bool hat_down  = (btns & (1u << VGP_BTN_DDOWN))  != 0;
        bool hat_left  = (btns & (1u << VGP_BTN_DLEFT))  != 0;
        bool hat_right = (btns & (1u << VGP_BTN_DRIGHT)) != 0;

        uint8_t hat_analog;
        if      (hat_up    && !hat_left && !hat_right) hat_analog =   0; // N
        else if (hat_up    && hat_right)               hat_analog =  32; // NE
        else if (hat_right && !hat_up   && !hat_down)  hat_analog =  64; // E
        else if (hat_down  && hat_right)               hat_analog =  96; // SE
        else if (hat_down  && !hat_left && !hat_right) hat_analog = 128; // S
        else if (hat_down  && hat_left)                hat_analog = 160; // SW
        else if (hat_left  && !hat_up   && !hat_down)  hat_analog = 192; // W
        else if (hat_up    && hat_left)                hat_analog = 224; // NW
        else                                           hat_analog = 255; // center/released

        joystate_struct.joy2_y = hat_analog;

        // 4 independent active-LOW button bits
        joystate_struct.button_mask =
            (!(btns & (1u << VGP_BTN_SOUTH)) << 4) |
            (!(btns & (1u << VGP_BTN_EAST))  << 5) |
            (!(btns & (1u << VGP_BTN_WEST))  << 6) |
            (!(btns & (1u << VGP_BTN_NORTH)) << 7);

    } else {
        // GAMEPAD, THROTTLE:
        // joy2 axes come from the HID path (already set in ghid_process_report
        // or update_joystate); here we just pass right stick through for
        // XInput/DS4 paths that call vgp_to_joystate directly.
        joystate_struct.joy2_x = vgp->axes[VGP_AXIS_RIGHT_X];
        joystate_struct.joy2_y = vgp->axes[VGP_AXIS_RIGHT_Y];

        // DOS buttons are active-LOW in bits 4-7.
        joystate_struct.button_mask =
            (!(btns & (1u << VGP_BTN_SOUTH)) << 4) |
            (!(btns & (1u << VGP_BTN_EAST))  << 5) |
            (!(btns & (1u << VGP_BTN_WEST))  << 6) |
            (!(btns & (1u << VGP_BTN_NORTH)) << 7);
    }
}

// Write slot 1 (joy2) from a second USB device.
// Only touches joy2_x/y and bits 6-7 of button_mask; joy1 and bits 4-5 are
// left exactly as the first device last wrote them.
static void vgp_to_joystate_joy2(const vgp_state_t* vgp) {
    uint16_t btns = vgp->buttons;

    joystate_struct.joy2_x = apply_deadzone(vgp->axes[VGP_AXIS_LEFT_X], joy_config.joy2_deadzone);
    uint8_t raw_y = vgp->axes[VGP_AXIS_LEFT_Y];
    if (joy_config.invert_joy2_y) raw_y = 255 - raw_y;
    joystate_struct.joy2_y = apply_deadzone(raw_y, joy_config.joy2_deadzone);

    // D-pad overrides joy2 axes
    if (btns & ((1u << VGP_BTN_DUP)   | (1u << VGP_BTN_DDOWN) |
                (1u << VGP_BTN_DLEFT) | (1u << VGP_BTN_DRIGHT))) {
        joystate_struct.joy2_x = (btns & (1u << VGP_BTN_DRIGHT)) ? 255 :
                                  (btns & (1u << VGP_BTN_DLEFT))  ? 0   : 127;
        joystate_struct.joy2_y = (btns & (1u << VGP_BTN_DDOWN))  ? 255 :
                                  (btns & (1u << VGP_BTN_DUP))    ? 0   : 127;
    }

    // Bits 6-7 = joy2 buttons (active-LOW); preserve bits 4-5 from device 1.
    // btn3/btn4 drive player-2 buttons in dual-player mode.
    joystate_struct.button_mask =
        (joystate_struct.button_mask & 0x30u) |  // preserve joy1 bits
        (!(btns & (1u << joy_config.btn3)) << 6) |
        (!(btns & (1u << joy_config.btn4)) << 7);
}

// Sony DS4 report layout detail https://www.psdevwiki.com/ps4/DS4-USB
typedef struct TU_ATTR_PACKED
{
    uint8_t x, y, z, rz; // joystick

    struct {
        uint8_t dpad     : 4; // (hat format, 0x08 is released, 0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW, 6=W, 7=NW)
        uint8_t square   : 1; // west
        uint8_t cross    : 1; // south
        uint8_t circle   : 1; // east
        uint8_t triangle : 1; // north
    };

    struct {
        uint8_t l1     : 1;
        uint8_t r1     : 1;
        uint8_t l2     : 1;
        uint8_t r2     : 1;
        uint8_t share  : 1;
        uint8_t option : 1;
        uint8_t l3     : 1;
        uint8_t r3     : 1;
    };

    struct {
        uint8_t ps      : 1; // playstation button
        uint8_t tpad    : 1; // track pad click
        uint8_t counter : 6; // +1 each report
    };

    uint8_t l2_trigger; // 0 released, 0xff fully pressed
    uint8_t r2_trigger; // as above

    //  uint16_t timestamp;
    //  uint8_t  battery;
    //
    //  int16_t gyro[3];  // x, y, z;
    //  int16_t accel[3]; // x, y, z

    // there is still lots more info

} sony_ds4_report_t;

// check if device is Sony DualShock 4
static inline bool is_sony_ds4(uint8_t dev_addr)
{
  uint16_t vid, pid;
  tuh_vid_pid_get(dev_addr, &vid, &pid);

  return ( (vid == 0x054c && (pid == 0x09cc || pid == 0x05c4)) // Sony DualShock4
           || (vid == 0x0f0d && pid == 0x005e)                 // Hori FC4
           || (vid == 0x0f0d && pid == 0x00ee)                 // Hori PS4 Mini (PS4-099U)
           || (vid == 0x1f4f && pid == 0x1002)                 // ASW GG xrd controller
         );
}

static inline void update_joystate(uint8_t dev_addr, uint8_t dpad, uint8_t x, uint8_t y, uint8_t z, uint8_t rz, uint8_t b1, uint8_t b2, uint8_t b3, uint8_t b4) {
    vgp_state_t vgp;
    vgp.buttons = 0;
    if (b1) vgp.buttons |= (1u << VGP_BTN_SOUTH);
    if (b2) vgp.buttons |= (1u << VGP_BTN_EAST);
    if (b3) vgp.buttons |= (1u << VGP_BTN_WEST);
    if (b4) vgp.buttons |= (1u << VGP_BTN_NORTH);
    // Hat switch → D-pad buttons (0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW, 6=W, 7=NW, 8=none)
    if (dpad == 0 || dpad == 1 || dpad == 7) vgp.buttons |= (1u << VGP_BTN_DUP);
    if (dpad == 3 || dpad == 4 || dpad == 5) vgp.buttons |= (1u << VGP_BTN_DDOWN);
    if (dpad == 1 || dpad == 2 || dpad == 3) vgp.buttons |= (1u << VGP_BTN_DRIGHT);
    if (dpad == 5 || dpad == 6 || dpad == 7) vgp.buttons |= (1u << VGP_BTN_DLEFT);
    vgp.axes[VGP_AXIS_LEFT_X]  = x;
    vgp.axes[VGP_AXIS_LEFT_Y]  = y;
    vgp.axes[VGP_AXIS_RIGHT_X] = z;
    vgp.axes[VGP_AXIS_RIGHT_Y] = rz;
    vgp.axes[VGP_AXIS_LT]      = 0;
    vgp.axes[VGP_AXIS_RT]      = 0;
    if (joy_get_slot(dev_addr) == 1)
        vgp_to_joystate_joy2(&vgp);
    else
        vgp_to_joystate(&vgp);
}

static inline void process_sony_ds4(uint8_t dev_addr, uint8_t const* report, uint16_t len)
{
    uint8_t const report_id = report[0];
    report++;
    len--;

    // all buttons state is stored in ID 1
    if (report_id == 1)
    {
        if (len < sizeof(sony_ds4_report_t)) return; // malformed / short report
        sony_ds4_report_t ds4_report;
        memcpy(&ds4_report, report, sizeof(ds4_report));

        update_joystate(dev_addr, ds4_report.dpad, ds4_report.x, ds4_report.y, ds4_report.z, ds4_report.rz,
                ds4_report.cross, ds4_report.circle, ds4_report.square, ds4_report.triangle);
        /*
        const char* dpad_str[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW", "none" };

        printf("(x, y, z, rz) = (%u, %u, %u, %u)\r\n", ds4_report.x, ds4_report.y, ds4_report.z, ds4_report.rz);
        printf("DPad = %s ", dpad_str[ds4_report.dpad]);

        if (ds4_report.square   ) printf("Square ");
        if (ds4_report.cross    ) printf("Cross ");
        if (ds4_report.circle   ) printf("Circle ");
        if (ds4_report.triangle ) printf("Triangle ");

        if (ds4_report.l1       ) printf("L1 ");
        if (ds4_report.r1       ) printf("R1 ");
        if (ds4_report.l2       ) printf("L2 ");
        if (ds4_report.r2       ) printf("R2 ");

        if (ds4_report.share    ) printf("Share ");
        if (ds4_report.option   ) printf("Option ");
        if (ds4_report.l3       ) printf("L3 ");
        if (ds4_report.r3       ) printf("R3 ");

        if (ds4_report.ps       ) printf("PS ");
        if (ds4_report.tpad     ) printf("TPad ");

        printf("\r\n");
        */
    }
}

// -----------------------------------------------------------------------
// Generic HID gamepad / joystick support
// Parses the raw HID report descriptor to find axis and button field
// positions, then extracts them from each incoming report and maps them
// to the four-axis / four-button DOS gameport joystate_struct.
// -----------------------------------------------------------------------

#define GHID_MAX_AXES 8   // X, Y, Z, Rx, Ry, Rz, Slider, Dial
#define GHID_MAX_RID  4   // report IDs tracked during parsing

typedef struct {
    uint16_t bit_offset;
    uint8_t  bit_size;
    int32_t  log_min;
    int32_t  log_max;
    uint8_t  usage;   // HID_USAGE_DESKTOP_X .. HID_USAGE_DESKTOP_RZ
} ghid_axis_t;

typedef struct {
    bool        valid;
    uint8_t     report_id;          // 0 = descriptor has no REPORT_ID items
    uint8_t     axis_count;
    ghid_axis_t axes[GHID_MAX_AXES];
    bool        has_hat;
    uint8_t     hat_report_id;      // report ID that carries the hat (may differ from report_id)
    uint16_t    hat_bit_offset;
    uint8_t     hat_bit_size;
    int32_t     hat_log_min;        // logical min for hat (centered = any value outside [min,min+7])
    int32_t     hat_log_max;        // logical max for hat
    uint16_t    button_bit_offset;  // bit offset of Button 1
    uint8_t     button_count;
} ghid_info_t;

static ghid_info_t ghid_info[CFG_TUH_HID];
// Per-instance cached hat value for devices where the hat switch is on a
// different report ID than the axes/buttons.  Initialised to 8 (centered).
static uint8_t ghid_cached_hat[CFG_TUH_HID];

// Extract a bit field from a byte array (HID uses LSB-first packing).
// Sign-extends the result when log_min is negative.
static int32_t ghid_extract_bits(const uint8_t* data, uint16_t bit_off,
                                  uint8_t bits, int32_t log_min) {
    uint32_t val = 0;
    for (uint8_t i = 0; i < bits && i < 32; i++) {
        uint16_t by = (bit_off + i) >> 3;
        uint8_t  bi = (bit_off + i) & 7;
        val |= (uint32_t)((data[by] >> bi) & 1u) << i;
    }
    if (log_min < 0 && bits < 32 && (val >> (bits - 1)))
        val |= ~((1u << bits) - 1u);
    return (int32_t)val;
}

// Map a raw axis value (within [log_min, log_max]) to [0, 255].
static uint8_t ghid_normalize(int32_t value, int32_t log_min, int32_t log_max) {
    if (log_max <= log_min) return 127;
    if (value < log_min) value = log_min;
    if (value > log_max) value = log_max;
    int64_t num = (int64_t)(value - log_min) * 255;
    int64_t den = (int64_t)(log_max - log_min);
    return (uint8_t)(num / den);
}

// Walk a HID report descriptor and populate ghid_info[instance] with the
// bit positions of axes, hat switch, and buttons for the first input report
// that contains gamepad-relevant fields.
static void ghid_parse_descriptor(uint8_t instance,
                                   const uint8_t* desc, uint16_t len) {
    ghid_info_t* info = &ghid_info[instance];
    memset(info, 0, sizeof(*info));

    // Global parser state
    uint16_t usage_page = 0;
    int32_t  log_min    = 0, log_max = 0;
    uint8_t  rpt_size   = 0, rpt_count = 0;

    // Per-report-ID bit-offset table (each report ID has its own bit cursor)
    uint8_t  rid_id [GHID_MAX_RID] = {0};
    uint16_t rid_off[GHID_MAX_RID] = {0};
    uint8_t  rid_n = 0;
    uint8_t  cur_rid = 0;   // 0 = no REPORT_ID items seen yet

    // Local state, cleared after every Main item
    uint8_t usages[16];
    uint8_t usage_cnt     = 0;
    uint8_t usage_min_val = 0, usage_max_val = 0;
    bool    has_range     = false;

    const uint8_t* p   = desc;
    const uint8_t* end = desc + len;

    while (p < end) {
        uint8_t hdr       = *p++;
        uint8_t size_code = hdr & 0x03;
        uint8_t itype     = (hdr >> 2) & 0x03;
        uint8_t itag      = hdr >> 4;
        uint8_t size      = (size_code == 3) ? 4 : size_code;
        if (p + size > end) break;  // malformed

        // Read data as little-endian unsigned integer
        uint32_t udata = 0;
        for (uint8_t i = 0; i < size; i++) udata |= (uint32_t)p[i] << (8 * i);
        p += size;

        if (itype == RI_TYPE_GLOBAL) {
            switch (itag) {
            case RI_GLOBAL_USAGE_PAGE:
                usage_page = (uint16_t)udata;
                break;
            case RI_GLOBAL_LOGICAL_MIN:
                if      (size == 1) log_min = (int32_t)(int8_t) (uint8_t) udata;
                else if (size == 2) log_min = (int32_t)(int16_t)(uint16_t)udata;
                else                log_min = (int32_t)udata;
                break;
            case RI_GLOBAL_LOGICAL_MAX: {
                // Sign-extend, but if result < log_min reinterpret as unsigned
                // (e.g. LOGICAL_MAX 0xFF = 255 in a [0..255] axis range)
                int32_t sv;
                if      (size == 1) sv = (int32_t)(int8_t) (uint8_t) udata;
                else if (size == 2) sv = (int32_t)(int16_t)(uint16_t)udata;
                else                sv = (int32_t)udata;
                if (sv < log_min) {
                    if      (size == 1) sv = (int32_t)(uint8_t) udata;
                    else if (size == 2) sv = (int32_t)(uint16_t)udata;
                }
                log_max = sv;
                break;
            }
            case RI_GLOBAL_REPORT_SIZE:  rpt_size  = (uint8_t)udata; break;
            case RI_GLOBAL_REPORT_COUNT: rpt_count = (uint8_t)udata; break;
            case RI_GLOBAL_REPORT_ID:
                cur_rid = (uint8_t)udata;
                // Register this ID in our per-ID offset table if new
                {
                    bool found = false;
                    for (uint8_t i = 0; i < rid_n; i++) {
                        if (rid_id[i] == cur_rid) { found = true; break; }
                    }
                    if (!found && rid_n < GHID_MAX_RID) {
                        rid_id[rid_n] = cur_rid;
                        rid_off[rid_n] = 0;
                        rid_n++;
                    }
                }
                break;
            }
        } else if (itype == RI_TYPE_LOCAL) {
            switch (itag) {
            case RI_LOCAL_USAGE:
                if (usage_cnt < 16) usages[usage_cnt++] = (uint8_t)udata;
                break;
            case RI_LOCAL_USAGE_MIN:
                usage_min_val = (uint8_t)udata;
                has_range     = true;
                break;
            case RI_LOCAL_USAGE_MAX:
                usage_max_val = (uint8_t)udata;
                break;
            }
        } else if (itype == RI_TYPE_MAIN) {
            if (itag == RI_MAIN_INPUT && rpt_size > 0 && rpt_count > 0) {
                bool is_const    = (udata & 0x01) != 0;
                bool is_variable = (udata & 0x02) != 0;

                // Locate (or create) the bit-offset entry for cur_rid
                uint16_t* poff = NULL;
                for (uint8_t i = 0; i < rid_n; i++) {
                    if (rid_id[i] == cur_rid) { poff = &rid_off[i]; break; }
                }
                // No REPORT_ID items at all: use slot 0
                if (poff == NULL) {
                    if (rid_n == 0) { rid_n = 1; rid_id[0] = 0; rid_off[0] = 0; }
                    poff = &rid_off[0];
                }

                if (!is_const && is_variable) {
                    for (uint8_t i = 0; i < rpt_count; i++) {
                        uint8_t usage;
                        if (usage_cnt > 0) {
                            usage = (i < usage_cnt) ? usages[i]
                                                     : usages[usage_cnt - 1];
                        } else if (has_range) {
                            usage = (uint8_t)(usage_min_val + i);
                        } else {
                            usage = 0;
                        }

                        uint16_t foff = *poff + (uint16_t)i * rpt_size;

                        if (usage_page == HID_USAGE_PAGE_DESKTOP) {
                            if (usage == HID_USAGE_DESKTOP_HAT_SWITCH) {
                                if (!info->has_hat) {
                                    info->has_hat        = true;
                                    info->hat_report_id  = cur_rid;
                                    info->hat_bit_offset = foff;
                                    info->hat_bit_size   = rpt_size;
                                    info->hat_log_min    = log_min;
                                    info->hat_log_max    = log_max;
                                    if (!info->valid) info->report_id = cur_rid;
                                    info->valid = true;
                                }
                            } else if ((usage >= HID_USAGE_DESKTOP_X &&
                                        usage <= HID_USAGE_DESKTOP_RZ) ||
                                       // Slider/Dial (throttle on HOTAS) — only accepted
                                       // if already locked to this report ID, so a throttle
                                       // unit on a separate report ID doesn't hijack the
                                       // stick's report and break button detection.
                                       ((usage == HID_USAGE_DESKTOP_SLIDER ||
                                         usage == HID_USAGE_DESKTOP_DIAL) &&
                                        (!info->valid || cur_rid == info->report_id))) {
                                if (info->axis_count < GHID_MAX_AXES) {
                                    ghid_axis_t* ax = &info->axes[info->axis_count++];
                                    ax->bit_offset = foff;
                                    ax->bit_size   = rpt_size;
                                    ax->log_min    = log_min;
                                    ax->log_max    = log_max;
                                    ax->usage      = usage;
                                    if (!info->valid) info->report_id = cur_rid;
                                    info->valid = true;
                                }
                            }
                        } else if (usage_page == HID_USAGE_PAGE_BUTTON) {
                            if (info->button_count == 0) {
                                info->button_bit_offset = foff;
                                if (!info->valid) info->report_id = cur_rid;
                                info->valid = true;
                            }
                            // Track by effective bit position so padding gaps between
                            // button groups (e.g. SNES adapters: B/Y/Sel/Start + 4-bit
                            // pad + A/X/L/R) are accounted for.  A simple increment
                            // would stop before the second group.
                            if (rpt_size > 0) {
                                uint8_t eff = (uint8_t)((foff - info->button_bit_offset)
                                                         / rpt_size) + 1u;
                                if (eff > info->button_count)
                                    info->button_count = eff;
                            }
                        }
                    }
                }
                // Advance bit offset (including constant / padding fields)
                *poff += (uint16_t)rpt_size * rpt_count;
            }
            // Clear local state after every Main item
            usage_cnt     = 0;
            has_range     = false;
            usage_min_val = usage_max_val = 0;
        }
    }
}

// Extract axes, hat, and buttons from a received HID report and call
// update_joystate() to push them into the DOS gameport emulation.
static void ghid_process_report(uint8_t dev_addr, uint8_t instance, const uint8_t* report,
                                 uint16_t len, uint8_t report_id) {
    ghid_info_t* info = &ghid_info[instance];
    if (!info->valid) return;

    // Per-instance cached hat value so a hat on a secondary report ID can be
    // merged with axis/button data arriving on the primary report ID.

    // If this report carries the hat and it's on a different report ID than
    // the main axes/buttons, extract the hat value, cache it, and return.
    // The cached value will be used the next time the main report arrives.
    if (info->has_hat &&
        info->hat_report_id != info->report_id &&
        report_id == info->hat_report_id) {
        uint16_t byte_end = (info->hat_bit_offset + info->hat_bit_size + 7u) >> 3;
        if (byte_end <= len) {
            int32_t hv = ghid_extract_bits(report, info->hat_bit_offset,
                                            info->hat_bit_size, 0);
            int32_t hdir2 = hv - info->hat_log_min;
            ghid_cached_hat[instance] = (hdir2 >= 0 && hdir2 <= 7) ? (uint8_t)hdir2 : 8u;
        }
        return;
    }

    // Only handle the primary report ID for axes/buttons
    if (info->report_id != report_id) return;

    // One-shot: print the actual received report length on first call so we
    // can verify the report covers the hat and button fields from the descriptor.
#ifdef GHID_DEBUG_DESCRIPTOR
    {
        static bool _len_printed[CFG_TUH_HID];
        if (!_len_printed[instance]) {
            _len_printed[instance] = true;
            printf("GHID first report: len=%u rpt_id=%u | hat_needs=%u btn_needs=%u\r\n",
                len, report_id,
                info->has_hat ? (unsigned)((info->hat_bit_offset + info->hat_bit_size + 7u) >> 3) : 0u,
                info->button_count > 0 ? (unsigned)((info->button_bit_offset + info->button_count + 7u) >> 3) : 0u);
        }
    }
#endif

    // Some devices (e.g. Saitek X45) send multiple report formats with the
    // same report ID but different lengths.  If this report is too short to
    // hold all the axes we parsed from the descriptor, it is a different
    // (secondary) report format and must be ignored entirely — reading partial
    // data from the wrong layout produces garbage axis values.
    for (uint8_t _i = 0; _i < info->axis_count; _i++) {
        uint16_t _bend = (info->axes[_i].bit_offset +
                          info->axes[_i].bit_size + 7u) >> 3;
        if (_bend > len) return;
    }

    uint8_t joy_x = 127, joy_y = 127, joy_z = 127, joy_rz = 127;
    // Track whether real analog X/Y data was found in this report.
    // If so, the hat switch must NOT override those axes (see update_joystate
    // call below).  Only devices with no real analog stick (pure D-pad
    // controllers) should have their hat converted to axis movement.
    bool has_axis_x = false, has_axis_y = false;

    // Separate slots so throttle profile can prioritize Slider over Z for joy2_x.
    uint8_t val_z = 127, val_rx = 127, val_ry = 127, val_rz = 127;
    uint8_t val_slider = 127, val_dial = 127;
    bool has_z = false, has_rx = false, has_ry = false, has_rz = false;
    bool has_slider = false, has_dial = false;

    for (uint8_t i = 0; i < info->axis_count; i++) {
        ghid_axis_t* ax = &info->axes[i];
        // Bounds check: all bits must fit within the received report
        uint16_t byte_end = (ax->bit_offset + ax->bit_size + 7u) >> 3;
        if (byte_end > len) continue;
        int32_t raw  = ghid_extract_bits(report, ax->bit_offset,
                                          ax->bit_size, ax->log_min);
        uint8_t norm = ghid_normalize(raw, ax->log_min, ax->log_max);
        switch (ax->usage) {
        // First X/Y wins: main stick axes come before any hat-as-axis entries
        // in a well-formed descriptor, so this prevents the POV hat (when
        // implemented as a second set of X/Y usages) from overwriting the
        // main stick values on every report.
        case HID_USAGE_DESKTOP_X:
            if (!has_axis_x) { joy_x = norm; has_axis_x = true; }
            break;
        case HID_USAGE_DESKTOP_Y:
            if (!has_axis_y) { joy_y = norm; has_axis_y = true; }
            break;
        case HID_USAGE_DESKTOP_Z:      if (!has_z)      { val_z      = norm; has_z      = true; } break;
        case HID_USAGE_DESKTOP_RX:     if (!has_rx)     { val_rx     = norm; has_rx     = true; } break;
        case HID_USAGE_DESKTOP_RY:     if (!has_ry)     { val_ry     = norm; has_ry     = true; } break;
        case HID_USAGE_DESKTOP_RZ:     if (!has_rz)     { val_rz     = norm; has_rz     = true; } break;
        case HID_USAGE_DESKTOP_SLIDER: if (!has_slider) { val_slider = norm; has_slider = true; } break;
        case HID_USAGE_DESKTOP_DIAL:   if (!has_dial)   { val_dial   = norm; has_dial   = true; } break;
        }
    }

    // Resolve joy2_x (joy_z) and joy2_y (joy_rz) based on runtime profile.
    // DOS gameport convention: joy2_x (axis 3) = rudder, joy2_y (axis 4) = throttle.
    if (joy_config.profile == JOY_PROFILE_THROTTLE) {
        if (!joy_config.swap_throttle_rudder) {
            // Standard: joy2_x = rudder (Rz), joy2_y = throttle (Slider)
            if      (has_rz)     joy_z  = val_rz;
            else if (has_z)      joy_z  = val_z;
            if      (has_slider) joy_rz = val_slider;
            else if (has_dial)   joy_rz = val_dial;
            else if (has_ry)     joy_rz = val_ry;
        } else {
            // Swapped: joy2_x = throttle (Slider), joy2_y = rudder (Rz)
            if      (has_slider) joy_z  = val_slider;
            else if (has_dial)   joy_z  = val_dial;
            if      (has_rz)     joy_rz = val_rz;
            else if (has_z)      joy_rz = val_z;
            else if (has_ry)     joy_rz = val_ry;
        }
        joy_rz = apply_deadzone(joy_rz, joy_config.joy2_deadzone);
    } else if (joy_config.profile == JOY_PROFILE_CH_FLIGHTSTICK) {
        // CH FlightStick Pro axis layout:
        //   joy2_x (axis 2) = twist/rudder → Rz
        //   joy2_y (axis 3) = throttle wheel → Slider or Z
        if      (has_rz)     joy_z  = val_rz;
        else if (has_rx)     joy_z  = val_rx;
        if      (has_slider) joy_rz = val_slider;
        else if (has_z)      joy_rz = val_z;
        else if (has_dial)   joy_rz = val_dial;
        else if (has_ry)     joy_rz = val_ry;
    } else if (joy_config.profile == JOY_PROFILE_THRUSTMASTER_FCS) {
        // Thrustmaster FCS axis layout:
        //   joy2_x (axis 2) = rudder → Rz
        //   joy2_y (axis 3) = hat analog (set in vgp_to_joystate, not here)
        if      (has_rz)     joy_z  = val_rz;
        else if (has_rx)     joy_z  = val_rx;
        // joy_rz unused — vgp_to_joystate overwrites joy2_y with hat analog
    } else {
        if      (has_z)      joy_z  = val_z;
        else if (has_rx)     joy_z  = val_rx;
        else if (has_slider) joy_z  = val_slider;
        else if (has_dial)   joy_z  = val_dial;
        if      (has_ry)     joy_rz = val_ry;
        else if (has_rz)     joy_rz = val_rz;
    }

    // --- AXIS DIAGNOSTIC ---
    // Uncomment GHID_DEBUG_AXES below, rebuild, and connect a serial terminal
    // (115200 baud) to see raw + normalized axis values as they change.
    // Useful for diagnosing wrong bit offsets, bad LOGICAL_MAX, or worn hardware.
//#define GHID_DEBUG_AXES
// Uncomment GHID_DEBUG_DESCRIPTOR to dump the raw HID descriptor hex and
// first-report length at mount time — useful for analysing a new device.
//#define GHID_DEBUG_DESCRIPTOR
#ifdef GHID_DEBUG_AXES
    {
        static uint8_t _px = 255, _py = 255, _pz = 255, _prz = 255;
        if (joy_x != _px || joy_y != _py || joy_z != _pz || joy_rz != _prz) {
            _px = joy_x; _py = joy_y; _pz = joy_z; _prz = joy_rz;
            printf("GHID axes norm: x=%3u y=%3u z=%3u rz=%3u\r\n",
                   joy_x, joy_y, joy_z, joy_rz);
            for (uint8_t _i = 0; _i < info->axis_count; _i++) {
                ghid_axis_t* _ax = &info->axes[_i];
                uint16_t _bend = (_ax->bit_offset + _ax->bit_size + 7u) >> 3;
                if (_bend > len) { printf("  ax[%u] OOB\r\n", _i); continue; }
                int32_t _raw = ghid_extract_bits(report, _ax->bit_offset,
                                                 _ax->bit_size, _ax->log_min);
                uint8_t _n   = ghid_normalize(_raw, _ax->log_min, _ax->log_max);
                printf("  ax[%u] usage=0x%02x raw=%ld norm=%u\r\n",
                       _i, _ax->usage, (long)_raw, _n);
            }
        }
    }
#endif

    // Hat switch → dpad value (0-7 directions, 8 = centered / released)
    uint8_t hat = 8;
    if (info->has_hat) {
        if (info->hat_report_id == info->report_id) {
            // Hat is on the same report as axes — extract directly
            uint16_t byte_end = (info->hat_bit_offset + info->hat_bit_size + 7u) >> 3;
            if (byte_end <= len) {
                int32_t hv = ghid_extract_bits(report, info->hat_bit_offset,
                                                info->hat_bit_size, 0);
#ifdef GHID_DEBUG_AXES
                {
                    static int32_t _phv = -999;
                    if (hv != _phv) { _phv = hv; printf("GHID hat raw=%ld\r\n", (long)hv); }
                }
#endif
                // Hat values run from log_min (N) to log_min+7 (NW), anything
                // outside that range (including null-state 0x42 flag) = centered.
                // Handles both 0-based (min=0, max=7) and 1-based (min=1, max=8) devices.
                int32_t hdir = hv - info->hat_log_min; // 0=N .. 7=NW, <0 or >7 = centered
                if (hdir >= 0 && hdir <= 7) hat = (uint8_t)hdir;
            }
        } else {
            // Hat is on a secondary report ID — use the cached value updated
            // by the secondary-report branch above.
            hat = ghid_cached_hat[instance];
        }
    }

    // --- BUTTON INDEX DISCOVERY ---
    // To find which HID index each physical button maps to on your controller:
    //   1. Connect a serial terminal (115200 baud) to the PicoGUS debug port
    //   2. Uncomment the GHID_DEBUG_BUTTONS line below, rebuild, and flash
    //   3. Press each button — serial output prints the index and a likely label
    //   4. Fill in ghid_button_order below using the named constants, then re-comment

    // Standard USB gamepad layout (hat switch present; Xbox / PlayStation / modern generic)
    // Index 0-based, matching the order buttons appear in the HID report.
    //   Btn  0 = South  — A (Xbox),  Cross    (PS),  B (Nintendo)
    //   Btn  1 = East   — B (Xbox),  Circle   (PS),  A (Nintendo)
    //   Btn  2 = West   — X (Xbox),  Square   (PS),  Y (Nintendo)
    //   Btn  3 = North  — Y (Xbox),  Triangle (PS),  X (Nintendo)
    //   Btn  4 = LB     — Left bumper / L1
    //   Btn  5 = RB     — Right bumper / R1
    //   Btn  6 = LT     — Left trigger / L2  (digital)
    //   Btn  7 = RT     — Right trigger / R2 (digital)
    //   Btn  8 = Select — Back / Share / Minus
    //   Btn  9 = Start  — Start / Options / Plus
    //   Btn 10 = L3     — Left stick click
    //   Btn 11 = R3     — Right stick click
    #define GHID_SOUTH   0
    #define GHID_EAST    1
    #define GHID_WEST    2
    #define GHID_NORTH   3
    #define GHID_LB      4
    #define GHID_RB      5
    #define GHID_LT      6
    #define GHID_RT      7
    #define GHID_SELECT  8
    #define GHID_START   9
    #define GHID_L3     10
    #define GHID_R3     11

    // SNES-style USB adapter layout (no hat switch; 12 buttons with 4-bit padding gap)
    // Bits 0-3: B, Y, Select, Start   (group 1)
    // Bits 4-7: [padding — not a real button]
    // Bits 8-11: A, X, L, R           (group 2)
    #define SNES_B       0
    #define SNES_Y       1
    #define SNES_SELECT  2
    #define SNES_START   3
    // 4-7: padding, do not use
    #define SNES_A       8
    #define SNES_X       9
    #define SNES_L      10
    #define SNES_R      11

    // Debug label table — standard layout names printed by GHID_DEBUG_BUTTONS
    static const char* const ghid_btn_labels[] = {
        "South(A/Cross/B)",   "East(B/Circle/A)",  "West(X/Square/Y)", "North(Y/Tri/X)",
        "LB/L1",              "RB/R1",              "LT/L2",            "RT/R2",
        "Select/Back",        "Start/Menu",         "L3",               "R3",
    };

//#define GHID_DEBUG_BUTTONS
#ifdef GHID_DEBUG_BUTTONS
    {
        static uint32_t prev_btn_state = 0xFFFFFFFF;
        uint32_t cur_btn_state = 0;
        uint8_t n = (info->button_count < 32) ? info->button_count : 32;
        uint16_t boff_d = info->button_bit_offset;
        for (uint8_t i = 0; i < n; i++) {
            uint16_t chk = (boff_d + i + 1u + 7u) >> 3;
            if (chk <= len)
                cur_btn_state |= (uint32_t)ghid_extract_bits(report, boff_d + i, 1, 0) << i;
        }
        if (cur_btn_state != prev_btn_state) {
            prev_btn_state = cur_btn_state;
            printf("GHID btn_count=%u pressed:", info->button_count);
            bool any = false;
            for (uint8_t i = 0; i < n; i++) {
                if ((cur_btn_state >> i) & 1u) {
                    if (i < (sizeof(ghid_btn_labels)/sizeof(ghid_btn_labels[0])))
                        printf(" %u(%s)", i, ghid_btn_labels[i]);
                    else
                        printf(" %u", i);
                    any = true;
                }
            }
            if (!any) printf(" (none)");
            printf("\r\n");
        }
    }
#endif

    // Resolve button indices from the layout table for the active slot.
    // layout=GAMEPAD uses btn1-4 directly (custom mapping); other layouts
    // use the fixed table in joy_btn_layout_table[].
    uint8_t slot = joy_get_slot(dev_addr);
    uint8_t layout = (slot == 1) ? joy_config.joy2_btn_layout : joy_config.joy1_btn_layout;
    uint8_t ghid_button_order[4];
    if (layout == JOY_BTN_LAYOUT_GAMEPAD) {
        ghid_button_order[0] = joy_config.btn1;
        ghid_button_order[1] = joy_config.btn2;
        ghid_button_order[2] = joy_config.btn3;
        ghid_button_order[3] = joy_config.btn4;
    } else {
        uint8_t l = (layout < JOY_BTN_LAYOUT_COUNT) ? layout : JOY_BTN_LAYOUT_GAMEPAD;
        ghid_button_order[0] = joy_btn_layout_table[l][0];
        ghid_button_order[1] = joy_btn_layout_table[l][1];
        ghid_button_order[2] = joy_btn_layout_table[l][2];
        ghid_button_order[3] = joy_btn_layout_table[l][3];
    }

    uint8_t b1 = 0, b2 = 0, b3 = 0, b4 = 0;
    if (info->button_count >= 1) {
        uint16_t boff = info->button_bit_offset;
        uint8_t max_idx = ghid_button_order[0];
        for (int i = 1; i < 4; i++)
            if (ghid_button_order[i] > max_idx) max_idx = ghid_button_order[i];
        uint16_t byte_end = (boff + (uint16_t)max_idx + 1u + 7u) >> 3;
        if (byte_end <= len) {
            if (info->button_count > ghid_button_order[0])
                b1 = (uint8_t)ghid_extract_bits(report, boff + ghid_button_order[0], 1, 0);
            if (info->button_count > ghid_button_order[1])
                b2 = (uint8_t)ghid_extract_bits(report, boff + ghid_button_order[1], 1, 0);
            if (info->button_count > ghid_button_order[2])
                b3 = (uint8_t)ghid_extract_bits(report, boff + ghid_button_order[2], 1, 0);
            if (info->button_count > ghid_button_order[3])
                b4 = (uint8_t)ghid_extract_bits(report, boff + ghid_button_order[3], 1, 0);
        }

        /* Latch the lowest-indexed button currently pressed.
         * Used by CMD_JOY_LAST_BTN for the pgusinit mapping wizard. */
        {
            uint8_t n = info->button_count < 32 ? info->button_count : 32;
            for (uint8_t _i = 0; _i < n; _i++) {
                uint16_t _bend = (boff + _i + 1u + 7u) >> 3;
                if (_bend > len) break;
                if (ghid_extract_bits(report, boff + _i, 1, 0)) {
                    joy_last_btn = _i;
                    break;
                }
            }
        }
    }

    // If the device has real analog X or Y axes, never let the hat override
    // them — the hat (POV, D-pad) is a secondary control on joysticks/HOTAS
    // and must not fight the analog stick.  Only pure D-pad controllers with
    // no analog stick should have their hat converted to axis movement.
    // Exception: CH and FCS profiles need the hat regardless, since they
    // encode it into button_mask / joy2_y rather than overriding the stick.
    bool pass_hat = !has_axis_x && !has_axis_y;
    if (joy_config.profile == JOY_PROFILE_CH_FLIGHTSTICK ||
        joy_config.profile == JOY_PROFILE_THRUSTMASTER_FCS) {
        pass_hat = true;
    }
    update_joystate(dev_addr, pass_hat ? hat : 8,
                    joy_x, joy_y, joy_z, joy_rz, b1, b2, b3, b4);
}
#endif

//--------------------------------------------------------------------+
// TinyUSB Callbacks
//--------------------------------------------------------------------+

// Invoked when device with hid interface is mounted
// Report descriptor is also available for use. tuh_hid_parse_report_descriptor()
// can be used to parse common/simple enough descriptor.
// Note: if report descriptor length > CFG_TUH_ENUMERATION_BUFSIZE, it will be skipped
// therefore report_desc = NULL, desc_len = 0
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* desc_report, uint16_t desc_len)
{
    printf("HID dev:%d inst:%d mounted\r\n", dev_addr, instance);

    // Interface protocol (hid_interface_protocol_enum_t)
    const char* protocol_str[] = { "n/a", "kbd", "mouse" };
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

    printf("HID protocol=%s\r\n", protocol_str[itf_protocol]);

    // By default host stack will use activate boot protocol on supported interface.
    // Therefore for this simple example, we only need to parse generic report descriptor (with built-in parser)
    if (itf_protocol == HID_ITF_PROTOCOL_NONE) {
        hid_info[instance].report_count = tuh_hid_parse_report_descriptor(hid_info[instance].report_info, MAX_REPORT, desc_report, desc_len);
        printf("HID has %u reports \r\n", hid_info[instance].report_count);
#ifdef USB_JOYSTICK
        if (desc_report && desc_len > 0) {
            // Dump raw descriptor bytes — enable with GHID_DEBUG_DESCRIPTOR above.
#ifdef GHID_DEBUG_DESCRIPTOR
            printf("DESC len=%u:\r\n", desc_len);
            for (uint16_t _di = 0; _di < desc_len; _di++) {
                printf("%02x", desc_report[_di]);
                if ((_di & 15) == 15 || _di == desc_len - 1) printf("\r\n");
                else printf(" ");
            }
#endif
            ghid_parse_descriptor(instance, desc_report, desc_len);
            if (ghid_info[instance].valid) {
                // Assign this device to a joystick slot (0=joy1, 1=joy2).
                // Only reset joystate to neutral when slot 0 is assigned so
                // a second device connecting doesn't disturb the first.
                uint8_t slot = joy_assign_slot(dev_addr);
                if (slot == 0) {
                    joystate_struct = (joystate_struct_t){127, 127, 127, 127, 0xf0};
                } else if (slot == 1) {
                    // Neutral joy2 axes; preserve joy1 state
                    joystate_struct.joy2_x = 127;
                    joystate_struct.joy2_y = 127;
                    joystate_struct.button_mask |= 0xC0u; // joy2 buttons released
                }
                ghid_cached_hat[instance] = 8; // hat centered until first report
                bool _fs = (ghid_info[instance].button_count > 16);
                printf("Generic %s: %u axes, %s hat, %u buttons (rpt_id=%u)\r\n",
                    _fs ? "flight stick" : "gamepad",
                    ghid_info[instance].axis_count,
                    ghid_info[instance].has_hat ? "has" : "no",
                    ghid_info[instance].button_count,
                    ghid_info[instance].report_id);
                if (ghid_info[instance].has_hat &&
                    ghid_info[instance].hat_report_id != ghid_info[instance].report_id) {
                    printf("  hat on secondary rpt_id=%u\r\n",
                        ghid_info[instance].hat_report_id);
                }
                // Print axis layout so descriptor parsing can be verified on a
                // serial terminal without recompiling with debug defines.
                for (uint8_t _a = 0; _a < ghid_info[instance].axis_count; _a++) {
                    ghid_axis_t* _ax = &ghid_info[instance].axes[_a];
                    printf("  axis[%u] usage=0x%02x off=%u bits=%u min=%ld max=%ld\r\n",
                        _a, _ax->usage, _ax->bit_offset, _ax->bit_size,
                        (long)_ax->log_min, (long)_ax->log_max);
                }
                if (ghid_info[instance].has_hat) {
                    printf("  hat off=%u bits=%u min=%ld max=%ld\r\n",
                        ghid_info[instance].hat_bit_offset,
                        ghid_info[instance].hat_bit_size,
                        (long)ghid_info[instance].hat_log_min,
                        (long)ghid_info[instance].hat_log_max);
                }
                printf("  buttons off=%u\r\n", ghid_info[instance].button_bit_offset);
            } else {
                printf("HID: no gamepad controls found in descriptor (not a joystick/gamepad?)\r\n");
            }
        }
#endif
    } else {
        // force boot protocol
        tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_BOOT);
    }

    // request to receive report
    // tuh_hid_report_received_cb() will be invoked when report is available
    if (!tuh_hid_receive_report(dev_addr, instance)) {
        printf("err(%d:%d) can't recv HID report\r\n", dev_addr, instance);
    }
}

// Invoked when device with hid interface is un-mounted
void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
    printf("HID dev:%d inst:%d unmounted\r\n", dev_addr, instance);
#ifdef USB_JOYSTICK
    if (ghid_info[instance].valid) {
        uint8_t slot = joy_get_slot(dev_addr);
        if (slot == 0) {
            // Primary device gone — neutral everything
            joystate_struct = (joystate_struct_t){127, 127, 127, 127, 0xf0};
        } else if (slot == 1) {
            // Secondary device gone — neutral joy2 only, preserve joy1
            joystate_struct.joy2_x = 127;
            joystate_struct.joy2_y = 127;
            joystate_struct.button_mask |= 0xC0u; // joy2 buttons released
        }
        joy_free_slot(dev_addr);
    }
    ghid_info[instance].valid = false;
#endif
}

//--------------------------------------------------------------------+
// Generic Report
//--------------------------------------------------------------------+
static inline void process_generic_report(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len)
{

#ifdef USB_JOYSTICK
    if (is_sony_ds4(dev_addr)) {
        process_sony_ds4(dev_addr, report, len);
        return;
    }
#endif

    uint8_t const rpt_count = hid_info[instance].report_count;
    tuh_hid_report_info_t* rpt_info_arr = hid_info[instance].report_info;
    tuh_hid_report_info_t* rpt_info = NULL;

    // Save original pointer in case we need it for the ghid fallback below.
    const uint8_t* const orig_report = report;
    const uint16_t       orig_len    = len;

    if (rpt_count == 1 && rpt_info_arr[0].report_id == 0) {
        // Simple report without report ID as 1st byte
        rpt_info = &rpt_info_arr[0];
    } else {
        // Composite report, 1st byte is report ID, data starts from 2nd byte
        uint8_t const rpt_id = report[0];

        // Find report id in the array
        for (uint8_t i=0; i<rpt_count; i++) {
            if (rpt_id == rpt_info_arr[i].report_id) {
                rpt_info = &rpt_info_arr[i];
                break;
            }
        }
        report++;
        len--;
    }

    if (!rpt_info) {
        printf("Couldn't find the report info for this report !\r\n");
#ifdef USB_JOYSTICK
        // TinyUSB's built-in parser couldn't identify this report, but our ghid
        // parser may have succeeded.  Try to handle it as a bare gamepad report
        // using whatever report_id we stored at mount time.
        if (ghid_info[instance].valid) {
            if (ghid_info[instance].report_id == 0) {
                // No report ID in descriptor – use the original unadvanced buffer.
                ghid_process_report(dev_addr, instance, orig_report, orig_len, 0);
            } else if (orig_len > 1 && orig_report[0] == ghid_info[instance].report_id) {
                // Report ID byte matches – use the already-advanced pointer (after ID).
                ghid_process_report(dev_addr, instance, report, len, ghid_info[instance].report_id);
            } else if (orig_len > 1 && ghid_info[instance].has_hat &&
                       orig_report[0] == ghid_info[instance].hat_report_id) {
                // Secondary hat report ID — forward so ghid_process_report can
                // cache the hat value even though TinyUSB didn't recognise it.
                ghid_process_report(dev_addr, instance, report, len, ghid_info[instance].hat_report_id);
            }
        }
#endif
        return;
    }

    // Mouse: handle via serial mouse driver and return early.
    if (rpt_info->usage_page == HID_USAGE_PAGE_DESKTOP &&
        rpt_info->usage == HID_USAGE_DESKTOP_MOUSE) {
        TU_LOG1("HID receive mouse report\r\n");
#ifdef USB_MOUSE
        sermouse_process_report((hid_mouse_report_t const*) report);
#endif
        return;
    }

    // Log known gamepad usages for debugging (TU_LOG1 is a no-op in release builds).
    if (rpt_info->usage_page == HID_USAGE_PAGE_DESKTOP) {
        if      (rpt_info->usage == HID_USAGE_DESKTOP_JOYSTICK) TU_LOG1("HID receive joystick report\r\n");
        else if (rpt_info->usage == HID_USAGE_DESKTOP_GAMEPAD)  TU_LOG1("HID receive gamepad report\r\n");
    }

#ifdef USB_JOYSTICK
    // Forward any non-mouse report to the generic gamepad handler when our
    // descriptor parser found gamepad fields (axes / hat / buttons).  This
    // covers Joystick, Gamepad, Multi-Axis Controller, and vendor usages so
    // that generic controllers that don't report JOYSTICK/GAMEPAD usage still
    // work correctly.
    if (ghid_info[instance].valid) {
        ghid_process_report(dev_addr, instance, report, len, rpt_info->report_id);
    }
#endif
}

// Invoked when received report from device via interrupt endpoint
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len)
{
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

    switch (itf_protocol) {
    case HID_ITF_PROTOCOL_MOUSE:
#ifdef USB_MOUSE
        sermouse_process_report((hid_mouse_report_t const*) report);
#endif
        break;

    case HID_ITF_PROTOCOL_KEYBOARD:
        // skip keyboard reports
        break;

    default:
        // Generic report requires matching ReportID and contents with previous parsed report info
        process_generic_report(dev_addr, instance, report, len);
        break;
    }

    // continue to request to receive report
    if (!tuh_hid_receive_report(dev_addr, instance)) {
        printf("err(%d:%d) can't recv HID report\r\n", dev_addr, instance);
    }
}

#ifdef USB_JOYSTICK
usbh_class_driver_t const* usbh_app_driver_get_cb(uint8_t* driver_count) {
    *driver_count = 1;
    return &usbh_xinput_driver;
}

static inline void update_joystate_xinput(uint8_t dev_addr, uint16_t wButtons, int16_t sThumbLX, int16_t sThumbLY, int16_t sThumbRX, int16_t sThumbRY, uint8_t bLeftTrigger, uint8_t bRightTrigger) {
    vgp_state_t vgp;
    vgp.buttons = 0;
    if (wButtons & XINPUT_GAMEPAD_A)              vgp.buttons |= (1u << VGP_BTN_SOUTH);
    if (wButtons & XINPUT_GAMEPAD_B)              vgp.buttons |= (1u << VGP_BTN_EAST);
    if (wButtons & XINPUT_GAMEPAD_X)              vgp.buttons |= (1u << VGP_BTN_WEST);
    if (wButtons & XINPUT_GAMEPAD_Y)              vgp.buttons |= (1u << VGP_BTN_NORTH);
    if (wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER)  vgp.buttons |= (1u << VGP_BTN_LB);
    if (wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) vgp.buttons |= (1u << VGP_BTN_RB);
    if (bLeftTrigger  > 64)                       vgp.buttons |= (1u << VGP_BTN_LT);
    if (bRightTrigger > 64)                       vgp.buttons |= (1u << VGP_BTN_RT);
    if (wButtons & XINPUT_GAMEPAD_BACK)           vgp.buttons |= (1u << VGP_BTN_SELECT);
    if (wButtons & XINPUT_GAMEPAD_START)          vgp.buttons |= (1u << VGP_BTN_START);
    if (wButtons & XINPUT_GAMEPAD_LEFT_THUMB)     vgp.buttons |= (1u << VGP_BTN_L3);
    if (wButtons & XINPUT_GAMEPAD_RIGHT_THUMB)    vgp.buttons |= (1u << VGP_BTN_R3);
    if (wButtons & XINPUT_GAMEPAD_DPAD_UP)        vgp.buttons |= (1u << VGP_BTN_DUP);
    if (wButtons & XINPUT_GAMEPAD_DPAD_DOWN)      vgp.buttons |= (1u << VGP_BTN_DDOWN);
    if (wButtons & XINPUT_GAMEPAD_DPAD_LEFT)      vgp.buttons |= (1u << VGP_BTN_DLEFT);
    if (wButtons & XINPUT_GAMEPAD_DPAD_RIGHT)     vgp.buttons |= (1u << VGP_BTN_DRIGHT);
    vgp.axes[VGP_AXIS_LEFT_X]  = (uint8_t)(((int32_t)sThumbLX  + 32768) >> 8);
    vgp.axes[VGP_AXIS_LEFT_Y]  = (uint8_t)((-(int32_t)sThumbLY + 32767) >> 8);
    vgp.axes[VGP_AXIS_RIGHT_X] = (uint8_t)(((int32_t)sThumbRX  + 32768) >> 8);
    vgp.axes[VGP_AXIS_RIGHT_Y] = (uint8_t)((-(int32_t)sThumbRY + 32767) >> 8);
    vgp.axes[VGP_AXIS_LT]      = bLeftTrigger;
    vgp.axes[VGP_AXIS_RT]      = bRightTrigger;
    if (joy_get_slot(dev_addr) == 1)
        vgp_to_joystate_joy2(&vgp);
    else
        vgp_to_joystate(&vgp);
}

void tuh_xinput_report_received_cb(uint8_t dev_addr, uint8_t instance, xinputh_interface_t const* xid_itf, uint16_t len)
{
    const xinput_gamepad_t *p = &xid_itf->pad;
    /*
    const char* type_str;
    switch (xid_itf->type)
    {
        case 1: type_str = "Xbox One";          break;
        case 2: type_str = "Xbox 360 Wireless"; break;
        case 3: type_str = "Xbox 360 Wired";    break;
        case 4: type_str = "Xbox OG";           break;
        default: type_str = "Unknown";
    }
    */

    if (xid_itf->connected && xid_itf->new_pad_data) {
        /*
        printf("[%02x, %02x], Type: %s, Buttons %04x, LT: %02x RT: %02x, LX: %d, LY: %d, RX: %d, RY: %d\n",
             dev_addr, instance, type_str, p->wButtons, p->bLeftTrigger, p->bRightTrigger, p->sThumbLX, p->sThumbLY, p->sThumbRX, p->sThumbRY);
        */
        update_joystate_xinput(dev_addr, p->wButtons, p->sThumbLX, p->sThumbLY, p->sThumbRX, p->sThumbRY, p->bLeftTrigger, p->bRightTrigger);
    }
    tuh_xinput_receive_report(dev_addr, instance);
}

void tuh_xinput_mount_cb(uint8_t dev_addr, uint8_t instance, const xinputh_interface_t *xinput_itf)
{
    printf("XINPUT MOUNTED %02x %d\n", dev_addr, instance);
    // Assign joystick slot
    uint8_t slot = joy_assign_slot(dev_addr);
    if (slot == 0) {
        joystate_struct = (joystate_struct_t){127, 127, 127, 127, 0xf0};
    } else if (slot == 1) {
        joystate_struct.joy2_x = 127;
        joystate_struct.joy2_y = 127;
        joystate_struct.button_mask |= 0xC0u;
    }
    // If this is a Xbox 360 Wireless controller we need to wait for a connection packet
    // on the in pipe before setting LEDs etc. So just start getting data until a controller is connected.
    if (xinput_itf->type == XBOX360_WIRELESS && xinput_itf->connected == false) {
        tuh_xinput_receive_report(dev_addr, instance);
        return;
    } else if (xinput_itf->type == XBOX360_WIRED) {
        /*
         * Some third-party Xbox 360-style controllers require this message to finish initialization.
         * Idea taken from Linux drivers/input/joystick/xpad.c
         */
        uint8_t dummy[20];
        tusb_control_request_t const request =
        {
            .bmRequestType_bit =
            {
                .recipient = TUSB_REQ_RCPT_INTERFACE,
                .type      = TUSB_REQ_TYPE_VENDOR,
                .direction = TUSB_DIR_IN
            },
            .bRequest = tu_htole16(0x01),
            .wValue   = tu_htole16(0x100),
            .wIndex   = tu_htole16(0x00),
            .wLength  = 20
        };
        tuh_xfer_t xfer =
        {
            .daddr       = dev_addr,
            .ep_addr     = 0,
            .setup       = &request,
            .buffer      = dummy,
            .complete_cb = NULL,
            .user_data   = 0
        };
        tuh_control_xfer(&xfer);
    }
    tuh_xinput_set_led(dev_addr, instance, 0, true);
    tuh_xinput_set_led(dev_addr, instance, 1, true);
    tuh_xinput_set_rumble(dev_addr, instance, 0, 0, true);
    tuh_xinput_receive_report(dev_addr, instance);
}

void tuh_xinput_umount_cb(uint8_t dev_addr, uint8_t instance)
{
    printf("XINPUT UNMOUNTED %02x %d\n", dev_addr, instance);
    uint8_t slot = joy_get_slot(dev_addr);
    if (slot == 0) {
        joystate_struct = (joystate_struct_t){127, 127, 127, 127, 0xf0};
    } else if (slot == 1) {
        joystate_struct.joy2_x = 127;
        joystate_struct.joy2_y = 127;
        joystate_struct.button_mask |= 0xC0u;
    }
    joy_free_slot(dev_addr);
}
#endif
