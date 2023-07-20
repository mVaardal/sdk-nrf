/*
 *  Copyright (c) 2021, PACKETCRAFT, INC.
 *
 *  SPDX-License-Identifier: LicenseRef-PCFT
 */

#ifndef _AUDIO_LC3_H_
#define AUDIO_LC3_H_

#include <zephyr/kernel.h>

int audio_lc3_init();
int audio_lc3_buffer_set(uint8_t *buf, size_t size);

#endif