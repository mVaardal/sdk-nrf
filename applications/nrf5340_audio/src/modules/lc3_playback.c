#include "lc3_playback.h"
#include <stdint.h>
#include <zephyr/sys/ring_buffer.h>
#include "sd_card.h"
#include "hw_codec.h"
#include "sw_codec_lc3.h"
#include "sw_codec_select.h"
#include "pcm_stream_channel_modifier.h"
#include "pcm_mix.h"
#include "audio_i2s.h"
#include "sw_codec_lc3.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(lc3_playback, CONFIG_MODULE_LC3_PLAYBACK_LOG_LEVEL);

#define RING_BUF_SIZE_1920_BYTES 1920
#define LC3_PLAYBACK_STACK_SIZE	 4096

/*File structure of the LC3 encoded files*/
struct lc3_binary_hdr_t {
	uint16_t file_id;	 /* Constant value, 0xCC1C */
	uint16_t hdr_size;	 /* Header size, 0x0012 */
	uint16_t sample_rate;	 /* Sample frequency / 100 */
	uint16_t bit_rate;	 /* Bit rate / 100 (total for all channels) */
	uint16_t channels;	 /* Number of channels */
	uint16_t frame_ms;	 /* Frame duration in ms * 100 */
	uint16_t rfu;		 /* RFU value */
	uint16_t signal_len_lsb; /* Number of samples in signal, 16 LSB */
	uint16_t signal_len_msb; /* Number of samples in signal, 16 MSB (>> 16) */
};

RING_BUF_DECLARE(m_ringbuf_sound_data_lc3, RING_BUF_SIZE_1920_BYTES);
K_SEM_DEFINE(m_sem_load_from_buf_lc3, 1, 1);
K_MUTEX_DEFINE(mtx_ringbuf);
K_SEM_DEFINE(m_sem_play_audiostream_from_file, 0, 1);
K_SEM_DEFINE(m_sem_stop_audiostream_from_file, 0, 1);
K_THREAD_STACK_DEFINE(lc3_playback_thread_stack, LC3_PLAYBACK_STACK_SIZE);

static struct lc3_binary_hdr_t lc3_file_header;
static uint16_t pcm_frame_size;
static bool lc3_playback_active;
static struct k_thread lc3_playback_thread_data;
static k_tid_t lc3_playback_thread_id;
static uint16_t lc3_frames_num;
static uint16_t lc3_frame_length;
static size_t lc3_frame_length_size = 2;
static uint16_t pcm_mono_write_size;

bool lc3_playback_is_active(void)
{
	return lc3_playback_active;
}

int lc3_playback_buffer_set(uint8_t *buf, size_t size)
{
	k_mutex_lock(&mtx_ringbuf, K_FOREVER);
	ring_buf_get(&m_ringbuf_sound_data_lc3, buf, size);
	k_mutex_unlock(&mtx_ringbuf);
	if (ring_buf_space_get(&m_ringbuf_sound_data_lc3) >= pcm_frame_size) {
		k_sem_give(&m_sem_load_from_buf_lc3);
	}
	return 0;
}

int lc3_playback_mix_with_stream(void *const pcm_a, size_t pcm_a_size)
{
	int ret;
	uint8_t pcm_b[pcm_frame_size];

	ret = lc3_playback_buffer_set(pcm_b, pcm_frame_size);
	if (ret < 0) {
		LOG_ERR("Error when loading pcm data into buffer. Ret: %d", ret);
		return ret;
	}
	pcm_mix(pcm_a, pcm_a_size, pcm_b, pcm_frame_size, B_MONO_INTO_A_STEREO_L);
	return 0;
}

static int lc3_playback_buffer_to_ringbuffer(uint8_t *buffer, size_t numbytes)
{
	static uint8_t *buf_ptr;

	k_mutex_lock(&mtx_ringbuf, K_FOREVER);
	numbytes = ring_buf_put_claim(&m_ringbuf_sound_data_lc3, &buf_ptr, numbytes);

	for (int i = 0; i < numbytes; i++) {
		buf_ptr[i] = buffer[i];
	}
	ring_buf_put_finish(&m_ringbuf_sound_data_lc3, numbytes);
	k_mutex_unlock(&mtx_ringbuf);
	return numbytes;
}

static int lc3_playback_header_read(const char *filename, const char *path_to_file)
{
	int ret;
	size_t lc3_file_header_size = sizeof(lc3_file_header);

	ret = sd_card_open(filename, path_to_file);
	if (ret < 0) {
		LOG_ERR("Error when trying to open file on SD card. Return value: %d", ret);
		return ret;
	}

	/* Read the header */
	ret = sd_card_read((char *)&lc3_file_header, &lc3_file_header_size);
	if (ret < 0) {
		LOG_ERR("Error when trying to peek at file segment on SD card. Return value: %d",
			ret);
		return ret;
	}

	return 0;
}

int lc3_playback_play(const char *filename, const char *path_to_file)
{
	int ret;

	ret = lc3_playback_header_read(filename, path_to_file);
	if (ret < 0) {
		LOG_ERR("Audio Lc3 header read failed. Return value: %d", ret);
		return ret;
	}

	pcm_frame_size = 2 * lc3_file_header.sample_rate * lc3_file_header.frame_ms / 1000;
	lc3_frames_num = 2 *
			 ((lc3_file_header.signal_len_msb << 16) + lc3_file_header.signal_len_lsb) /
			 pcm_frame_size;

	k_sem_give(&m_sem_play_audiostream_from_file);
	k_sem_take(&m_sem_stop_audiostream_from_file, K_FOREVER);
	ret = sd_card_close();
	if (ret < 0) {
		LOG_ERR("Error when closing file. Return value: %d", ret);
		return ret;
	}
	lc3_playback_active = false;
	return 0;
}

static int lc3_playback_play_wav(const char *filename, const char *path_to_file,
				 uint32_t frame_duration_ms, uint8_t bit_depth,
				 enum Sample_rates sample_rate, enum Audio_channels audio_ch)
{
	int ret;

	size_t pcm_wav_mono_frame_size = frame_duration_ms * sample_rate * bit_depth / 8;
	pcm_frame_size = pcm_wav_mono_frame_size;
	uint8_t pcm_wav_mono_frame[pcm_wav_mono_frame_size];
	ret = sd_card_open(filename, path_to_file);
	if (ret < 0) {
		LOG_ERR("Error when trying to open file on SD card. Return value: %d", ret);
		return ret;
	}

	lc3_playback_active = true;
	while (lc3_playback_active) {

		ret = sd_card_read(pcm_wav_mono_frame, &pcm_wav_mono_frame_size);
		if (ret < 0) {
			LOG_ERR("Error when trying to read file on SD card. Return value: %d", ret);
			return ret;
		}
		if (pcm_wav_mono_frame_size < pcm_frame_size) {
			lc3_playback_active = false;
		}
		/* Wait until there is enough space in the ringbuffer */
		if (lc3_playback_active) {
			k_sem_take(&m_sem_load_from_buf_lc3, K_FOREVER);
			lc3_playback_buffer_to_ringbuffer(pcm_wav_mono_frame,
							  pcm_wav_mono_frame_size);
		}
	}
	ret = sd_card_close();
	if (ret < 0) {
		LOG_ERR("Error when closing file. Return value: %d", ret);
		return ret;
	}
	lc3_playback_active = false;
	return 0;
}


int lc3_playback_play_lc3(){
	int ret;
	uint8_t pcm_mono_frame[pcm_frame_size];

	lc3_playback_active = true;
	for (uint32_t i = 0; i < lc3_frames_num; i++) {
		/* Skip the frame length info to get to the audio data */
		ret = sd_card_read((char *)&lc3_frame_length, &lc3_frame_length_size);
		if (ret < 0) {
			LOG_ERR("Error when trying to skip file on SD card. Return value: %d", ret);
			return ret;
		}
		uint8_t lc3_frame[lc3_frame_length];
		size_t lc3_fr_len = lc3_frame_length;
		/* Read the audio data frame to be encoded */
		ret = sd_card_read((char *)lc3_frame, &lc3_fr_len);
		if (ret < 0) {
			LOG_ERR("Something went wrong when reading from SD card. Return value: %d",
				ret);
			return ret;
		}
		/* Decode audio data frame*/
		ret = sw_codec_lc3_dec_run((char *)lc3_frame, lc3_frame_length, pcm_frame_size, 1,
					   pcm_mono_frame, &pcm_mono_write_size, false);
		if (ret < 0) {
			LOG_ERR("Error when running decoder. Return value: %d\n", ret);
			return ret;
		}
		/* Wait until there is enough space in the ringbuffer */
		k_sem_take(&m_sem_load_from_buf_lc3, K_FOREVER);
		lc3_playback_buffer_to_ringbuffer((char *)pcm_mono_frame, pcm_mono_write_size);
	}
	return 0;
}

static void lc3_playback_thread(void *arg1, void *arg2, void *arg3)
{
	while (!sw_codec_is_initialized()) {
		k_msleep(100);
	}
	k_sem_take(&m_sem_play_audiostream_from_file, K_FOREVER);
	// lc3_playback_play_wav("whitney_48k_mono.wav", "", 10, 16, SAMPLE_RATE_48K, AUDIO_CH_MONO);
	lc3_playback_play_lc3();
	k_sem_give(&m_sem_stop_audiostream_from_file);
}

int lc3_playback_init(void)
{
	int ret;

	lc3_playback_thread_id =
		k_thread_create(&lc3_playback_thread_data, lc3_playback_thread_stack,
				LC3_PLAYBACK_STACK_SIZE, (k_thread_entry_t)lc3_playback_thread,
				NULL, NULL, NULL, K_PRIO_PREEMPT(4), 0, K_NO_WAIT);
	ret = k_thread_name_set(lc3_playback_thread_id, "lc3_playback");
	if (ret < 0) {
		LOG_ERR("Failed");
		return ret;
	}
	return 0;
}
