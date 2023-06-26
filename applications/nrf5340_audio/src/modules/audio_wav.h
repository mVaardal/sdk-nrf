#include <stddef.h>
#include <stdint.h>
#ifndef _AUDIO_WAV_H_
#define _AUDIO_WAV_H_

size_t sd_card_to_buffer(int numbytes);
int play_file_from_sd(const char *filename, char *path_to_file);
void audio_wav_i2s_callback(uint32_t frame_start_ts, uint32_t *rx_buf_released, uint32_t const *tx_buf_released);

#endif