/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _AUDIO_LC3_H_
#define _AUDIO_LC3_H_
#include <stdint.h>

/**@brief Callback function for the i2s interrupts
 * @param[in] frame_start_ts Frame size in milliseconds
 * @param[in] sample_rate Sample rate
 * @param[in] bit_depth Bit depth
 * @param[in] channels Number of channels
 * @return  0 on success.
 *      Otherwise, error from underlying drivers.
*/
// void audio_lc3_i2s_callback(uint32_t *rx_buf_released, uint32_t const *tx_buf_released);
void audio_lc3_i2s_callback(uint32_t frame_start_ts, uint32_t *rx_buf_released, uint32_t const *tx_buf_released);


/**@brief Play audio from lc3 file
 * @param[in] filename Name of file
 * @param[in] path_to_file Path to file
 * @return  0 on success.
 *      Otherwise, error from underlying drivers.
*/
int audio_lc3_play(const char *filename, const char *path_to_file);

/**@brief Initialize sw codec decoder
 * @param[in] frame_duration_ms Frame size in milliseconds
 * @param[in] sample_rate Sample rate
 * @param[in] bit_depth Bit depth
 * @param[in] channels Number of channels
 * @return  0 on success.
 *      Otherwise, error from underlying drivers.
*/
int audio_lc3_sw_codec_decoder_init(uint16_t frame_duration_ms, uint32_t sample_rate, uint8_t bit_depth, uint8_t channels);

/**@brief Initialize the audio_lc3_play function
 * @param[in] filename Name of file
 * @param[in] path_to_file Path to file
 * @return  0 on success.
 *      Otherwise, error from underlying drivers.
*/
int audio_lc3_play_init(const char *filename, const char *path_to_file);


#endif