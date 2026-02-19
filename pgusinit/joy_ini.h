/*
 * joy_ini.h - Joystick INI profile loader for pgusinit.
 *
 * Reads a [joystick] INI file and sends the settings to the PicoGUS firmware
 * over the ISA control port using CMD_JOY_* commands.
 *
 * Also provides joy_ini_set_profile(), joy_ini_print_status(), and the
 * button name lookup table used by joy_wizard.h.
 */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <conio.h>
#include <dos.h>
#include "ini_parser.h"
#include "../common/picogus.h"

/* -----------------------------------------------------------------------
 * Button name -> HID index lookup
 *
 * Prefixed names allow controller-specific references without needing to
 * know the raw HID index.  All prefixes resolve to the same underlying
 * HID index — the prefix is just a human-readable alias.
 *
 * Generic / positional (works for any modern gamepad):
 *   south east west north  lb rb lt rt  select start  l3 r3
 *
 * Xbox / generic USB gamepad:
 *   xbox_a xbox_b xbox_x xbox_y  xbox_lb xbox_rb xbox_lt xbox_rt
 *   xbox_back xbox_start xbox_l3 xbox_r3
 *
 * PlayStation:
 *   ps_cross ps_circle ps_square ps_triangle
 *   ps_l1 ps_r1 ps_l2 ps_r2  ps_share ps_options ps_l3 ps_r3
 *
 * SNES USB adapter (1-based HID indices):
 *   snes_b snes_y snes_select snes_start  snes_a snes_x snes_l snes_r
 *
 * Raw numeric index also accepted (e.g. btn1=7).
 * ----------------------------------------------------------------------- */
typedef struct { const char* name; uint8_t idx; } joy_btn_name_t;

static const joy_btn_name_t joy_btn_names[] = {
    /* --- generic positional --- */
    {"south",        0}, {"east",         1}, {"west",         2}, {"north",        3},
    {"lb",           4}, {"rb",           5}, {"lt",           6}, {"rt",           7},
    {"select",       8}, {"back",         8}, {"share",        8},
    {"start",        9}, {"options",      9},
    {"l3",          10}, {"r3",          11},

    /* --- Xbox / generic USB gamepad --- */
    {"xbox_a",       0}, {"xbox_b",       1}, {"xbox_x",       2}, {"xbox_y",       3},
    {"xbox_lb",      4}, {"xbox_rb",      5}, {"xbox_lt",      6}, {"xbox_rt",      7},
    {"xbox_back",    8}, {"xbox_select",  8}, {"xbox_start",   9}, {"xbox_options", 9},
    {"xbox_l3",     10}, {"xbox_r3",     11},

    /* --- PlayStation --- */
    {"ps_cross",     0}, {"ps_a",         0},
    {"ps_circle",    1}, {"ps_b",         1},
    {"ps_square",    2}, {"ps_x",         2},
    {"ps_triangle",  3}, {"ps_y",         3},
    {"ps_l1",        4}, {"ps_r1",        5},
    {"ps_l2",        6}, {"ps_r2",        7},
    {"ps_share",     8}, {"ps_select",    8},
    {"ps_options",   9}, {"ps_start",     9},
    {"ps_l3",       10}, {"ps_r3",       11},

    /* --- legacy short aliases --- */
    {"a",            0}, {"cross",        0},
    {"b",            1}, {"circle",       1},
    {"x",            2}, {"square",       2},
    {"y",            3}, {"triangle",     3},
    {"l1",           4}, {"r1",           5},
    {"l2",           6}, {"r2",           7},

    /* --- SNES USB adapter (1-based HID indices) --- */
    {"snes_b",       1}, {"snes_y",       2},
    {"snes_select",  3}, {"snes_start",   4},
    {"snes_a",       9}, {"snes_x",      10},
    {"snes_l",      11}, {"snes_r",      12}
};
#define JOY_BTN_NAME_COUNT (sizeof(joy_btn_names)/sizeof(joy_btn_names[0]))

static uint8_t joy_ini_parse_btn(const char* s) {
    unsigned int i;
    char lower[32];
    unsigned int len = (unsigned int)strlen(s);
    if (len >= sizeof(lower)) len = sizeof(lower) - 1;
    for (i = 0; i < len; i++) lower[i] = (char)tolower((unsigned char)s[i]);
    lower[len] = '\0';
    for (i = 0; i < JOY_BTN_NAME_COUNT; i++) {
        if (strcmp(lower, joy_btn_names[i].name) == 0)
            return joy_btn_names[i].idx;
    }
    return (uint8_t)atoi(s);
}

/* -----------------------------------------------------------------------
 * Profile name -> CMD_JOY_PROFILE value (axis routing only)
 * ----------------------------------------------------------------------- */
static uint8_t joy_ini_parse_profile(const char* s) {
    if (!stricmp(s, "gamepad"))          return 0;
    if (!stricmp(s, "throttle"))         return 1;
    if (!stricmp(s, "hotas"))            return 1;
    if (!stricmp(s, "ch_flightstick"))   return 2;
    if (!stricmp(s, "ch"))               return 2;
    if (!stricmp(s, "fcs"))              return 3;
    if (!stricmp(s, "thrustmaster"))     return 3;
    if (!stricmp(s, "thrustmaster_fcs")) return 3;
    return (uint8_t)atoi(s);
}

/* -----------------------------------------------------------------------
 * Layout name -> JOY_BTN_LAYOUT_* value (USB controller button table)
 * ----------------------------------------------------------------------- */
static uint8_t joy_ini_parse_layout(const char* s) {
    if (!stricmp(s, "gamepad")) return 0;
    if (!stricmp(s, "snes"))    return 1;
    return (uint8_t)atoi(s);
}

/* -----------------------------------------------------------------------
 * Send a single-byte CMD_JOY_* command to the firmware.
 * ----------------------------------------------------------------------- */
static void joy_ini_send(uint8_t cmd, uint8_t value) {
    outp(CONTROL_PORT, 0xCC);
    outp(CONTROL_PORT, cmd);
    outp(DATA_PORT_HIGH, value);
}

/* -----------------------------------------------------------------------
 * Parse and apply all settings from the [joystick] section.
 * ----------------------------------------------------------------------- */
static void joy_ini_apply(const ini_file_t* ini) {
    const char* val;
    const char* b1val;
    const char* b2val;
    const char* b3val;
    const char* b4val;
    int inv;

    b1val = ini_get(ini, "joystick", "btn1");
    b2val = ini_get(ini, "joystick", "btn2");
    b3val = ini_get(ini, "joystick", "btn3");
    b4val = ini_get(ini, "joystick", "btn4");

    if (b1val) joy_ini_send(CMD_JOY_BTN1, joy_ini_parse_btn(b1val));
    if (b2val) joy_ini_send(CMD_JOY_BTN2, joy_ini_parse_btn(b2val));
    if (b3val) joy_ini_send(CMD_JOY_BTN3, joy_ini_parse_btn(b3val));
    if (b4val) joy_ini_send(CMD_JOY_BTN4, joy_ini_parse_btn(b4val));

    /* If any custom btn was specified, force layout=gamepad so the firmware
     * uses btn1-4 directly, overriding any layout table. */
    if (b1val || b2val || b3val || b4val) {
        joy_ini_send(CMD_JOY_JOY1_LAYOUT, 0); /* JOY_BTN_LAYOUT_GAMEPAD */
    } else {
        /* No custom btns — apply layout if specified */
        val = ini_get(ini, "joystick", "joy1_btn_layout");
        if (val) joy_ini_send(CMD_JOY_JOY1_LAYOUT, joy_ini_parse_layout(val));
    }

    val = ini_get(ini, "joystick", "joy2_btn_layout");
    if (val) joy_ini_send(CMD_JOY_JOY2_LAYOUT, joy_ini_parse_layout(val));

    val = ini_get(ini, "joystick", "invert_joy1_y");
    if (val) {
        inv = (!strcmp(val, "1") || !stricmp(val, "true") || !stricmp(val, "yes")) ? 1 : 0;
        joy_ini_send(CMD_JOY_JOY1_FLAGS, (uint8_t)inv);
    }

    val = ini_get(ini, "joystick", "invert_joy2_y");
    if (val) {
        inv = (!strcmp(val, "1") || !stricmp(val, "true") || !stricmp(val, "yes")) ? 1 : 0;
        joy_ini_send(CMD_JOY_JOY2_FLAGS, (uint8_t)inv);
    }
}

/* -----------------------------------------------------------------------
 * Load a joystick INI file and apply all settings to the firmware.
 * ----------------------------------------------------------------------- */
static int joy_ini_load(const char* path) {
    ini_file_t ini;
    const char* val;
    int ret;
    int sw;

    ret = ini_load(&ini, path);
    if (ret != 0) {
        fprintf(stderr, "ERROR: cannot open joystick INI file: %s\n", path);
        return ret;
    }

    val = ini_get(&ini, "joystick", "profile");
    if (val) joy_ini_send(CMD_JOY_PROFILE, joy_ini_parse_profile(val));

    val = ini_get(&ini, "joystick", "joy1_deadzone");
    if (val) joy_ini_send(CMD_JOY_JOY1_DZ, (uint8_t)atoi(val));

    val = ini_get(&ini, "joystick", "joy2_deadzone");
    if (val) joy_ini_send(CMD_JOY_JOY2_DZ, (uint8_t)atoi(val));

    val = ini_get(&ini, "joystick", "swap_throttle_rudder");
    if (val) {
        sw = (!strcmp(val, "1") || !stricmp(val, "true") || !stricmp(val, "yes")) ? 1 : 0;
        joy_ini_send(CMD_JOY_FLAGS, (uint8_t)sw);
    }

    joy_ini_apply(&ini);
    ini_free(&ini);
    printf("Joystick config loaded from %s. Use /save to persist.\n", path);
    return 0;
}

/* -----------------------------------------------------------------------
 * Apply a named axis-routing profile preset.
 * ----------------------------------------------------------------------- */
static int joy_ini_set_profile(const char* name) {
    uint8_t profile_id;

    if      (!stricmp(name, "gamepad") || !stricmp(name, "4button") ||
             !stricmp(name, "4btn"))                    profile_id = 0;
    else if (!stricmp(name, "throttle") ||
             !stricmp(name, "hotas"))                   profile_id = 1;
    else if (!stricmp(name, "ch") ||
             !stricmp(name, "ch_flightstick") ||
             !stricmp(name, "chfs"))                    profile_id = 2;
    else if (!stricmp(name, "fcs") ||
             !stricmp(name, "thrustmaster") ||
             !stricmp(name, "tmfcs"))                   profile_id = 3;
    else {
        fprintf(stderr, "ERROR: unknown joystick profile '%s'\n", name);
        fprintf(stderr, "Valid profiles: gamepad, throttle, ch_flightstick, fcs\n");
        return -1;
    }

    joy_ini_send(CMD_JOY_PROFILE, profile_id);
    printf("Joystick profile set to '%s'. Use /save to persist.\n", name);
    return 0;
}

static const char* joy_profile_names[] = {
    "gamepad", "throttle", "ch_flightstick", "fcs"
};
#define JOY_PROFILE_NAME_COUNT (sizeof(joy_profile_names)/sizeof(joy_profile_names[0]))

static const char* joy_layout_names[] = { "gamepad", "snes" };
#define JOY_LAYOUT_NAME_COUNT (sizeof(joy_layout_names)/sizeof(joy_layout_names[0]))

/* -----------------------------------------------------------------------
 * Print current joystick config by reading back from firmware.
 * ----------------------------------------------------------------------- */
static void joy_ini_print_status(void) {
    uint8_t profile, joy1_dz, joy2_dz, flags;
    uint8_t b1, b2, b3, b4;
    uint8_t joy1_layout, joy1_flags, joy2_layout, joy2_flags;
    uint8_t eff1, eff2, eff3, eff4;

    /* Each read needs its own knock + select + read sequence */
    #define RD(cmd) (outp(CONTROL_PORT, 0xCC), outp(CONTROL_PORT, (cmd)), inp(DATA_PORT_HIGH))
    profile     = RD(CMD_JOY_PROFILE);
    joy1_dz     = RD(CMD_JOY_JOY1_DZ);
    joy2_dz     = RD(CMD_JOY_JOY2_DZ);
    flags       = RD(CMD_JOY_FLAGS);
    b1          = RD(CMD_JOY_BTN1);
    b2          = RD(CMD_JOY_BTN2);
    b3          = RD(CMD_JOY_BTN3);
    b4          = RD(CMD_JOY_BTN4);
    joy1_layout = RD(CMD_JOY_JOY1_LAYOUT);
    joy1_flags  = RD(CMD_JOY_JOY1_FLAGS);
    joy2_layout = RD(CMD_JOY_JOY2_LAYOUT);
    joy2_flags  = RD(CMD_JOY_JOY2_FLAGS);
    eff1        = RD(CMD_JOY_EFF_BTN1);
    eff2        = RD(CMD_JOY_EFF_BTN2);
    eff3        = RD(CMD_JOY_EFF_BTN3);
    eff4        = RD(CMD_JOY_EFF_BTN4);
    #undef RD

    printf("Joystick config:\n");
    printf("  profile=%s  joy1_deadzone=%u  joy2_deadzone=%u  swap_throttle=%u\n",
        profile < JOY_PROFILE_NAME_COUNT ? joy_profile_names[profile] : "?",
        (unsigned)joy1_dz, (unsigned)joy2_dz, (unsigned)(flags & 1));
    printf("  joy1_btn_layout=%s  invert_joy1_y=%u\n",
        joy1_layout < JOY_LAYOUT_NAME_COUNT ? joy_layout_names[joy1_layout] : "?",
        (unsigned)(joy1_flags & 1));
    printf("  joy2_btn_layout=%s  invert_joy2_y=%u\n",
        joy2_layout < JOY_LAYOUT_NAME_COUNT ? joy_layout_names[joy2_layout] : "?",
        (unsigned)(joy2_flags & 1));
    printf("  stored custom: btn1=%u  btn2=%u  btn3=%u  btn4=%u\n",
        (unsigned)b1, (unsigned)b2, (unsigned)b3, (unsigned)b4);
    printf("  effective joy1: btn1=%u  btn2=%u  btn3=%u  btn4=%u\n",
        (unsigned)eff1, (unsigned)eff2, (unsigned)eff3, (unsigned)eff4);
    printf("  (in dual-player mode btn3/btn4 become player-2 btn1/btn2)\n");
}
