#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t joy1_x;
    uint8_t joy1_y;
    uint8_t joy2_x;
    uint8_t joy2_y;
    uint8_t button_mask;
} joystate_struct_t;

// ============================================================
// Profile constants — axis routing only.
// Button mapping is handled separately via btn_layout (see below).
// ============================================================
#define JOY_PROFILE_GAMEPAD          0  // Standard gamepad: right stick → joy2, 4 buttons
#define JOY_PROFILE_THROTTLE         1  // HOTAS: Slider→joy2_y (throttle), Rz→joy2_x (rudder)
#define JOY_PROFILE_CH_FLIGHTSTICK   2  // CH FlightStick Pro (4-bit combo button/hat encoding)
#define JOY_PROFILE_THRUSTMASTER_FCS 3  // Thrustmaster FCS (hat encoded as analog on joy2_y)
#define JOY_PROFILE_COUNT            4

// ============================================================
// Button layout constants — USB controller type → DOS button mapping.
//
// A layout selects a complete 4-button index table that maps DOS buttons
// 1-4 to HID button indices for a specific USB controller type.
// joy1_btn_layout applies to player 1; joy2_btn_layout to player 2.
//
// Built-in layouts:
//
//   GAMEPAD (0) — Standard USB gamepad (Xbox / PlayStation / modern generic)
//     DOS btn1 = idx 0  South  — A (Xbox), Cross (PS), B (Nintendo)
//     DOS btn2 = idx 1  East   — B (Xbox), Circle (PS), A (Nintendo)
//     DOS btn3 = idx 2  West   — X (Xbox), Square (PS), Y (Nintendo)
//     DOS btn4 = idx 3  North  — Y (Xbox), Triangle (PS), X (Nintendo)
//
//   SNES (1) — SNES USB adapter (1-based HID indices: B=1,Y=2,Sel=3,Start=4,A=9,X=10,L=11,R=12)
//     DOS btn1 = idx 1  B  (south position — matches Gravis btn1)
//     DOS btn2 = idx 9  A  (east position  — matches Gravis btn2)
//     DOS btn3 = idx 2  Y  (west position  — matches Gravis btn3)
//     DOS btn4 = idx 10 X  (north position — matches Gravis btn4)
//
// Both layouts produce the same Gravis Gamepad-compatible button positions.
// Custom per-button mapping via btn1-4 overrides any layout table when set
// through the INI (pgusinit forces layout=GAMEPAD when btn1-4 are present).
// ============================================================
#define JOY_BTN_LAYOUT_GAMEPAD  0
#define JOY_BTN_LAYOUT_SNES     1
#define JOY_BTN_LAYOUT_COUNT    2

// HID button index constants (named aliases for use in joy.ini / build flags)
#define JOY_BTN_SOUTH    0
#define JOY_BTN_EAST     1
#define JOY_BTN_WEST     2
#define JOY_BTN_NORTH    3
#define JOY_BTN_LB       4
#define JOY_BTN_RB       5
#define JOY_BTN_LT       6
#define JOY_BTN_RT       7
#define JOY_BTN_SELECT   8   // also SNES A
#define JOY_BTN_START    9   // also SNES X
#define JOY_BTN_L3      10   // also SNES L
#define JOY_BTN_R3      11   // also SNES R
#define JOY_BTN_NONE  0xFF   // disable this button slot

// Button index table — indexed by [layout][dos_btn 0-3]
// Keep in sync with JOY_BTN_LAYOUT_* constants above.
static const uint8_t joy_btn_layout_table[JOY_BTN_LAYOUT_COUNT][4] = {
    { 0, 1, 2, 3 },   // GAMEPAD: South, East, West, North
    { 1, 9, 2, 10 },  // SNES:    B, A, Y, X (1-based HID indices)
};

// ============================================================
// Runtime configuration struct
// ============================================================
typedef struct {
    uint8_t profile;              // JOY_PROFILE_* — axis routing only
    uint8_t joy1_deadzone;        // joy1 center dead zone (0=off, 1-127)
                                  //   single-player: main stick X/Y
                                  //   dual-player:   player 1 stick X/Y
    uint8_t joy2_deadzone;        // joy2 center dead zone (0=off, 1-127)
                                  //   single-player: right stick / throttle axis
                                  //   dual-player:   player 2 stick X/Y
    bool    swap_throttle_rudder; // swap joy2_x/joy2_y in THROTTLE profile
    uint8_t btn1;                 // custom HID index → DOS button 1 (overrides layout table when set via INI)
    uint8_t btn2;                 // custom HID index → DOS button 2
    uint8_t btn3;                 // custom HID index → DOS button 3 / player-2 btn1 in dual
    uint8_t btn4;                 // custom HID index → DOS button 4 / player-2 btn2 in dual
    uint8_t joy1_btn_layout;      // JOY_BTN_LAYOUT_* for player 1 controller
    uint8_t joy2_btn_layout;      // JOY_BTN_LAYOUT_* for player 2 controller
    bool    invert_joy1_y;        // flip joy1_y
    bool    invert_joy2_y;        // flip joy2_y
} joy_config_t;

// ============================================================
// USER CONFIGURATION — edit these before building.
// ============================================================

#define JOY_PROFILE_DEFAULT           JOY_PROFILE_GAMEPAD
#define JOY_JOY1_DEADZONE_DEFAULT     0
#define JOY_JOY2_DEADZONE_DEFAULT     0
#define JOY_SWAP_THROTTLE_DEFAULT     false
// Custom button indices — used when layout=GAMEPAD (overridden by layout table otherwise)
#define JOY_BTN1_DEFAULT              JOY_BTN_SOUTH
#define JOY_BTN2_DEFAULT              JOY_BTN_EAST
#define JOY_BTN3_DEFAULT              JOY_BTN_WEST
#define JOY_BTN4_DEFAULT              JOY_BTN_NORTH
// Button layout — selects which USB controller type table to use per player
#define JOY_JOY1_BTN_LAYOUT_DEFAULT   JOY_BTN_LAYOUT_GAMEPAD
#define JOY_JOY2_BTN_LAYOUT_DEFAULT   JOY_BTN_LAYOUT_GAMEPAD
#define JOY_INVERT_JOY1_Y_DEFAULT     false
#define JOY_INVERT_JOY2_Y_DEFAULT     false

// ============================================================

#ifdef __cplusplus
extern "C" {
#endif

void usb_hotplug_task(void);

#ifdef __cplusplus
}
#endif
