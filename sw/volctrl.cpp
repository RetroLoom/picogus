/*
 *  Copyright (C) 2025  Daniel Arnold
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <string.h>
#include <stdio.h>

#include "flash_settings.h"
#include "pico/stdlib.h"
#include "volctrl.h"
#include "clamp.h"
#include "flash_settings.h"
extern Settings settings;

int32_t opl_volume = 0x10000; // default 1.0x volume
int32_t sb_volume = 0x10000; // default 1.0x volume
int32_t cd_audio_volume = 0x10000; // default 1.0x volume
int32_t main_volume = 0x10000; // default 1.0x volume

int32_t set_volume_scale (uint8_t percent) {
     if (percent > 100)
        percent = 100;

    int32_t volume = (percent * 65536) / 100;
    
    if (percent < 1) 
        volume = 0;

    return volume;
}

int32_t scale_sample (int32_t sample, int32_t scale, int clamp) {
    sample = (sample * scale) >> 16;

    if (clamp)
        clamp16(sample);

    return sample;
}

void set_volume(uint16_t mode) {

    switch (mode){
        case MODE_MAINVOL:
            main_volume = set_volume_scale(settings.Volume.mainvol);
            break;
        case MODE_OPLVOL:
            opl_volume = set_volume_scale(settings.Volume.oplvol);
            break;
        case MODE_SBVOL:
            sb_volume = set_volume_scale(settings.Volume.sbvol);
            break;
        case MODE_CDVOL:
            cd_audio_volume = set_volume_scale(settings.Volume.sbvol);
            break;
    }   
}
