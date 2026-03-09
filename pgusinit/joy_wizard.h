/*
 * joy_wizard.h - Interactive joystick button mapping wizard for pgusinit.
 *
 * Guides the user through pressing each button on their USB controller to
 * assign it to DOS gameport buttons 1-4.  Uses CMD_JOY_LAST_BTN to read
 * the last HID button index pressed on the firmware side.
 *
 * The target mapping matches the Gravis Gamepad layout:
 *   DOS btn1 = south face button  (Xbox:A  PS:Cross    SNES:B  Gravis:1)
 *   DOS btn2 = east  face button  (Xbox:B  PS:Circle   SNES:A  Gravis:2)
 *   DOS btn3 = west  face button  (Xbox:X  PS:Square   SNES:Y  Gravis:3)
 *   DOS btn4 = north face button  (Xbox:Y  PS:Triangle SNES:X  Gravis:4)
 *
 * Usage:  pgusinit /joywizard p1|p2
 */
#pragma once
#include <stdio.h>
#include <conio.h>
#include <dos.h>
#include <string.h>
#include "../common/picogus.h"
#include "joy_ini.h"

/* Poll CMD_JOY_LAST_BTN until a button is pressed or Escape is hit.
 * Returns the HID button index (0-31), or 0xFF on Escape / timeout. */
static uint8_t joy_wizard_wait_btn(void) {
    uint8_t idx;
    int key;

    /* Drain any stale latch first */
    outp(CONTROL_PORT, 0xCC);
    outp(CONTROL_PORT, CMD_JOY_LAST_BTN);
    inp(DATA_PORT_HIGH);

    printf("  (press button, or Esc to cancel)");
    fflush(stdout);

    for (;;) {
        /* Check for keypress */
        if (kbhit()) {
            key = getch();
            if (key == 27) { /* Escape */
                printf("\n");
                return 0xFF;
            }
        }

        /* Poll firmware */
        outp(CONTROL_PORT, 0xCC);
        outp(CONTROL_PORT, CMD_JOY_LAST_BTN);
        idx = inp(DATA_PORT_HIGH);
        if (idx != 0xFF) {
            printf(" -> HID index %u\n", (unsigned)idx);
            /* Small debounce delay */
            delay(300);
            /* Drain latch again so next prompt starts clean */
            outp(CONTROL_PORT, 0xCC);
            outp(CONTROL_PORT, CMD_JOY_LAST_BTN);
            inp(DATA_PORT_HIGH);
            return idx;
        }

        delay(20);
    }
}

/*
 * Run the mapping wizard for one player slot (1 or 2).
 * Sets btn1-4 (and layout=gamepad so they are used directly).
 * Returns 0 on success, -1 if cancelled.
 *
 * Button position reference (press the button at the described position):
 *   DOS btn1 = SOUTH / bottom face  — Xbox:A  PS:Cross    SNES:B  Gravis:1
 *   DOS btn2 = EAST  / right  face  — Xbox:B  PS:Circle   SNES:A  Gravis:2
 *   DOS btn3 = WEST  / left   face  — Xbox:X  PS:Square   SNES:Y  Gravis:3
 *   DOS btn4 = NORTH / top    face  — Xbox:Y  PS:Triangle SNES:X  Gravis:4
 */
static int joy_wizard_run(int player) {
    uint8_t btns[4];
    int i;
    /* Descriptions by physical position — controller labels in parentheses */
    static const char* const dos_btn_names[] = {
        "DOS button 1 - SOUTH / bottom  (Xbox:A  PS:Cross    SNES:B  Gravis:1)",
        "DOS button 2 - EAST  / right   (Xbox:B  PS:Circle   SNES:A  Gravis:2)",
        "DOS button 3 - WEST  / left    (Xbox:X  PS:Square   SNES:Y  Gravis:3)",
        "DOS button 4 - NORTH / top     (Xbox:Y  PS:Triangle SNES:X  Gravis:4)",
    };
    static const uint8_t btn_cmds[] = {
        CMD_JOY_BTN1, CMD_JOY_BTN2, CMD_JOY_BTN3, CMD_JOY_BTN4
    };

    printf("Joystick mapping wizard - Player %d\n", player);
    printf("Make sure your controller is plugged in.\n");
    printf("Press Esc at any prompt to cancel.\n\n");
    printf("Press the button at each described position on your controller.\n");
    printf("This will match the Gravis Gamepad layout used by DOS games.\n\n");

    for (i = 0; i < 4; i++) {
        printf("%s:\n", dos_btn_names[i]);
        btns[i] = joy_wizard_wait_btn();
        if (btns[i] == 0xFF) {
            printf("Wizard cancelled.\n");
            return -1;
        }
    }

    /* Send the mappings */
    outp(CONTROL_PORT, 0xCC);
    for (i = 0; i < 4; i++) {
        joy_ini_send(btn_cmds[i], btns[i]);
    }

    /* Set layout=gamepad so btn1-4 are used directly */
    if (player == 2) {
        joy_ini_send(CMD_JOY_JOY2_LAYOUT, 0); /* JOY_BTN_LAYOUT_GAMEPAD */
    } else {
        joy_ini_send(CMD_JOY_JOY1_LAYOUT, 0); /* JOY_BTN_LAYOUT_GAMEPAD */
    }

    printf("\nMapping set:\n");
    printf("  DOS btn1=%u  btn2=%u  btn3=%u  btn4=%u\n",
        (unsigned)btns[0], (unsigned)btns[1],
        (unsigned)btns[2], (unsigned)btns[3]);
    printf("Use /save to persist.\n");
    return 0;
}
