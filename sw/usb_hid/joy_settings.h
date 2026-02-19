/*
 * joy_settings.h — Joystick config ISA command handlers and flash integration.
 *
 * Keeps all joystick-specific protocol handling out of picogus.cpp so that
 * upstream merges only need minimal changes to the core files.
 *
 * Firmware side only — not included by pgusinit.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "joy.h"
#include "../../common/picogus.h"

// -----------------------------------------------------------------------
// Flash-persisted joystick settings
//
// Layout (must stay stable — do not reorder fields):
//   [0]  magic_lo  \  0x4A ('J') + 0x59 ('Y') = "JY"
//   [1]  magic_hi  /
//   [2]  profile
//   [3]  joy1_deadzone
//   [4]  joy2_deadzone
//   [5]  flags           bit0=swap_throttle_rudder
//   [6]  btn1
//   [7]  btn2
//   [8]  btn3
//   [9]  btn4
//   [10] joy1_btn_layout  JOY_BTN_LAYOUT_*
//   [11] joy1_flags       bit0=invert_joy1_y
//   [12] joy2_btn_layout  JOY_BTN_LAYOUT_*
//   [13] joy2_flags       bit0=invert_joy2_y
//   [14..N] reserved (zero)
// -----------------------------------------------------------------------
#define JOY_FLASH_MAGIC_LO   0x4A  // 'J'
#define JOY_FLASH_MAGIC_HI   0x59  // 'Y'
#define JOY_FLASH_BLOCK_SIZE 20

static inline void joy_config_to_flash(const joy_config_t* cfg, uint8_t* block) {
    block[0]  = JOY_FLASH_MAGIC_LO;
    block[1]  = JOY_FLASH_MAGIC_HI;
    block[2]  = cfg->profile;
    block[3]  = cfg->joy1_deadzone;
    block[4]  = cfg->joy2_deadzone;
    block[5]  = cfg->swap_throttle_rudder ? 1u : 0u;
    block[6]  = cfg->btn1;
    block[7]  = cfg->btn2;
    block[8]  = cfg->btn3;
    block[9]  = cfg->btn4;
    block[10] = cfg->joy1_btn_layout;
    block[11] = cfg->invert_joy1_y ? 1u : 0u;
    block[12] = cfg->joy2_btn_layout;
    block[13] = cfg->invert_joy2_y ? 1u : 0u;
    for (uint8_t i = 14; i < JOY_FLASH_BLOCK_SIZE; i++) block[i] = 0;
}

static inline bool joy_config_from_flash(joy_config_t* cfg, const uint8_t* block) {
    if (block[0] != JOY_FLASH_MAGIC_LO || block[1] != JOY_FLASH_MAGIC_HI)
        return false;
    cfg->profile              = block[2];
    cfg->joy1_deadzone        = block[3];
    cfg->joy2_deadzone        = block[4];
    cfg->swap_throttle_rudder = (block[5] & 1u) != 0;
    cfg->btn1                 = block[6];
    cfg->btn2                 = block[7];
    cfg->btn3                 = block[8];
    cfg->btn4                 = block[9];
    cfg->joy1_btn_layout      = block[10];
    cfg->invert_joy1_y        = (block[11] & 1u) != 0;
    cfg->joy2_btn_layout      = block[12];
    cfg->invert_joy2_y        = (block[13] & 1u) != 0;
    return true;
}

// -----------------------------------------------------------------------
// ISA command dispatch
// -----------------------------------------------------------------------

extern joy_config_t joy_config;

static inline bool joy_settings_write(uint8_t cmd, uint8_t value) {
    switch (cmd) {
    case CMD_JOY_PROFILE:       joy_config.profile = value;                           return true;
    case CMD_JOY_JOY1_DZ:       joy_config.joy1_deadzone = value;                     return true;
    case CMD_JOY_JOY2_DZ:       joy_config.joy2_deadzone = value;                     return true;
    case CMD_JOY_FLAGS:         joy_config.swap_throttle_rudder = (value & 1u) != 0; return true;
    case CMD_JOY_BTN1:          joy_config.btn1 = value;                             return true;
    case CMD_JOY_BTN2:          joy_config.btn2 = value;                             return true;
    case CMD_JOY_BTN3:          joy_config.btn3 = value;                             return true;
    case CMD_JOY_BTN4:          joy_config.btn4 = value;                             return true;
    case CMD_JOY_JOY1_LAYOUT:   joy_config.joy1_btn_layout = value;                  return true;
    case CMD_JOY_JOY1_FLAGS:    joy_config.invert_joy1_y = (value & 1u) != 0;       return true;
    case CMD_JOY_JOY2_LAYOUT:   joy_config.joy2_btn_layout = value;                  return true;
    case CMD_JOY_JOY2_FLAGS:    joy_config.invert_joy2_y = (value & 1u) != 0;       return true;
    default:                    return false;
    }
}

static inline uint8_t joy_settings_read(uint8_t cmd) {
    switch (cmd) {
    case CMD_JOY_PROFILE:       return joy_config.profile;
    case CMD_JOY_JOY1_DZ:       return joy_config.joy1_deadzone;
    case CMD_JOY_JOY2_DZ:       return joy_config.joy2_deadzone;
    case CMD_JOY_FLAGS:         return joy_config.swap_throttle_rudder ? 1u : 0u;
    case CMD_JOY_BTN1:          return joy_config.btn1;
    case CMD_JOY_BTN2:          return joy_config.btn2;
    case CMD_JOY_BTN3:          return joy_config.btn3;
    case CMD_JOY_BTN4:          return joy_config.btn4;
    case CMD_JOY_JOY1_LAYOUT:   return joy_config.joy1_btn_layout;
    case CMD_JOY_JOY1_FLAGS:    return joy_config.invert_joy1_y ? 1u : 0u;
    case CMD_JOY_JOY2_LAYOUT:   return joy_config.joy2_btn_layout;
    case CMD_JOY_JOY2_FLAGS:    return joy_config.invert_joy2_y ? 1u : 0u;
    case CMD_JOY_EFF_BTN1:
        return (joy_config.joy1_btn_layout == JOY_BTN_LAYOUT_GAMEPAD)
            ? joy_config.btn1
            : joy_btn_layout_table[joy_config.joy1_btn_layout][0];
    case CMD_JOY_EFF_BTN2:
        return (joy_config.joy1_btn_layout == JOY_BTN_LAYOUT_GAMEPAD)
            ? joy_config.btn2
            : joy_btn_layout_table[joy_config.joy1_btn_layout][1];
    case CMD_JOY_EFF_BTN3:
        return (joy_config.joy1_btn_layout == JOY_BTN_LAYOUT_GAMEPAD)
            ? joy_config.btn3
            : joy_btn_layout_table[joy_config.joy1_btn_layout][2];
    case CMD_JOY_EFF_BTN4:
        return (joy_config.joy1_btn_layout == JOY_BTN_LAYOUT_GAMEPAD)
            ? joy_config.btn4
            : joy_btn_layout_table[joy_config.joy1_btn_layout][3];
    default:                    return 0xFF;
    }
}

#ifdef __cplusplus
}
#endif
