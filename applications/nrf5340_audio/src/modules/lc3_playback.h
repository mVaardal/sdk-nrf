/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _LC3_PLAYBACK_H_
#define _LC3_PLAYBACK_H_

#include <zephyr/kernel.h>

/**@brief   Figure out whether or not the lc3_playback module is active
 * @retval  true, if active. false, otherwise
 */
bool lc3_playback_is_active(void);

/**@brief   Mix pcm data from lc3_playback module with audio stream out
 * param pcm_a Buffer into which to mix pcm data from lc3_module
 * param size Size of input buffer
 * @retval  0 on success
 */
int lc3_playback_mix_with_stream(void *const pcm_a, size_t pcm_a_size);

/**@brief   Initialize lc3_playback module. Create lc3_playback thread
 * @retval  0 on success
 *		Otherwise, error from underlying drivers
 */
int lc3_playback_init(void);

#endif
