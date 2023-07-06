#include "audio_lc3.h"
#include <stdint.h>
#include <zephyr/sys/ring_buffer.h>
#include "sd_card.h"
#include "hw_codec.h"
#include "sw_codec_lc3.h"
#include <stdlib.h>
#include "pcm_stream_channel_modifier.h"
#include "audio_i2s.h"

#define AUDIO_LC3_DEFAULT_VOLUME 30
#define AUDIO_LC3_BIT_DEPTH 16
#define I2S_16BIT_SAMPLE_NUM 768
#define I2S_BUF_BYTES (I2S_16BIT_SAMPLE_NUM * 2)

/*File structure of the LC3 encoded files*/
typedef struct {
	uint16_t fileId; /* Constant value, 0xCC1C */
	uint16_t hdrSize; /* Header size, 0x0012 */
	uint16_t sampleRate_divided100; /* Sample frequency / 100 */
	uint16_t bitRate; /* Bit rate / 100 (total for all channels) */
	uint16_t channels; /* Number of channels */
	uint16_t frameMs_times100; /* Frame duration in ms * 100 */
	uint16_t rfu; /* RFU value */
	uint16_t signalLen; /* Number of samples in signal, 16 LSB */
	uint16_t signalLenRed; /* Number of samples in signal, 16 MSB (>> 16) */
} lc3BinaryHdr_t;

static lc3BinaryHdr_t header;
static size_t header_size = sizeof(lc3BinaryHdr_t);
static uint16_t lc3_frame_length;
// switched from 16 to char!
static char *pcm_mono_frame;

static uint16_t m_i2s_tx_buf_a[I2S_16BIT_SAMPLE_NUM], m_i2s_rx_buf_a[I2S_16BIT_SAMPLE_NUM], m_i2s_tx_buf_b[I2S_16BIT_SAMPLE_NUM], m_i2s_rx_buf_b[I2S_16BIT_SAMPLE_NUM];

RING_BUF_DECLARE(m_ringbuf_sound_data_lc3, 3840);
K_SEM_DEFINE(m_sem_load_from_buf_lc3, 0, 1);


int audio_lc3_set_up_i2s_transmission(){
	// Start I2S transmission by setting up both buffersets
	ring_buf_get(&m_ringbuf_sound_data_lc3, (uint8_t *)m_i2s_tx_buf_a, I2S_BUF_BYTES);
	audio_i2s_start((const uint8_t *)m_i2s_tx_buf_a, (uint32_t *)m_i2s_rx_buf_a);
	ring_buf_get(&m_ringbuf_sound_data_lc3, (uint8_t *)m_i2s_tx_buf_b, I2S_BUF_BYTES);
	audio_i2s_set_next_buf((const uint8_t *)m_i2s_tx_buf_b, (uint32_t *)m_i2s_rx_buf_b);
	return 0;
}

void audio_lc3_i2s_callback(uint32_t frame_start_ts, uint32_t *rx_buf_released, uint32_t const *tx_buf_released)
{
	// Update the I2S buffers by reading from the ringbuffer
	if((uint16_t *)tx_buf_released == m_i2s_tx_buf_a) {
		ring_buf_get(&m_ringbuf_sound_data_lc3, (uint8_t *)m_i2s_tx_buf_a, I2S_BUF_BYTES);
		audio_i2s_set_next_buf((const uint8_t *)m_i2s_tx_buf_a, (uint32_t *)m_i2s_rx_buf_a);
	} else if((uint16_t *)tx_buf_released == m_i2s_tx_buf_b) {
		ring_buf_get(&m_ringbuf_sound_data_lc3, (uint8_t *)m_i2s_tx_buf_b, I2S_BUF_BYTES);
		audio_i2s_set_next_buf((const uint8_t *)m_i2s_tx_buf_b, (uint32_t *)m_i2s_rx_buf_b);
	} else {
		printk("Should not happen! 0x%x\n", (int)tx_buf_released);
		return;
	}

	// Check the current free space in the buffer.
	// If more than half the buffer is free we should move more data from the SD card
	// printk("Leftover space in ringbuffer is: %d\n", ring_buf_space_get(&m_ringbuf_sound_data_lc3));
	if(ring_buf_space_get(&m_ringbuf_sound_data_lc3) > 1920) {
		k_sem_give(&m_sem_load_from_buf_lc3);
	}
}

int audio_lc3_buffer_to_ringbuffer(uint8_t *buffer, size_t numbytes){
	static uint8_t *buf_ptr;

	numbytes = ring_buf_put_claim(&m_ringbuf_sound_data_lc3, &buf_ptr, numbytes);

	for (int i = 0; i<numbytes; i++){
		buf_ptr[i] = buffer[i];
	}
	ring_buf_put_finish(&m_ringbuf_sound_data_lc3, numbytes);
	return numbytes;
}

int audio_lc3_play(const char *filename, const char *path_to_file)
{
	int ret;
	lc3BinaryHdr_t header;
	size_t header_size = sizeof(lc3BinaryHdr_t);
	char lc3_frame[100];
	uint16_t lc3_frame_length;
	size_t lc3_frame_length_size = sizeof(lc3_frame_length);
	// uint16_t *pcm_mono_frame;
	uint16_t pcm_mono_frame[480];
	uint16_t pcm_mono_frame_size;
	uint16_t pcm_mono_write_size;
	char pcm_stereo_frame[1920];
	size_t pcm_stereo_write_size;
	uint32_t num_samples;

	/* First, open file on SD card */
	ret = sd_card_segment_open(filename, path_to_file);
	if (ret < 0) {
		printk("Error when trying to open file on SD card. Return value: %d", ret);
		return ret;
	}

	/* Then, read the header */
	ret = sd_card_segment_read((char *)&header, &header_size);
	if (ret < 0) {
		printk("Error when trying to open file on SD card. Return value: %d", ret);
		return ret;
	}

	ret = audio_lc3_sw_codec_decoder_init(header.frameMs_times100 / 100,
					(uint32_t)(header.sampleRate_divided100 * 100),
					AUDIO_LC3_BIT_DEPTH, (char)header.channels);
	if (ret < 0){
		printk("Error when initializing audio sw codec\n");
		return ret;
	}

	/* I don't really want to pass a pointer to lc3_frame_length_size in the below function... */
	sd_card_segment_peek((char *)&lc3_frame_length, &lc3_frame_length_size);

	ret = audio_lc3_set_up_i2s_transmission();
	if (ret < 0){
		printk("Error when setting up audio i2s transmission. Error nr: %d\n", ret);
		return ret;
	}

	hw_codec_volume_set(AUDIO_LC3_DEFAULT_VOLUME);

	num_samples = (header.signalLenRed << 16) + header.signalLen;
	for (uint32_t i = 0; i < num_samples; i++) {
		/* Read/skip the frame length info to get to the audio data */
		// printk("Header.channels 1: %d\n", header.channels);
		size_t frs = 100;
		printk("lc3_frame_length_size: %d\n", lc3_frame_length_size);
		ret = sd_card_segment_read((char *)&lc3_frame_length, &lc3_frame_length_size);
		if (ret < 0) {
			printk("Error when trying to read file on SD card. Return value: %d", ret);
			return ret;
		}
		printk("lc3_frame_length: %d\n", lc3_frame_length);
		ret = sd_card_segment_read((char *)lc3_frame, &frs);
		if(ret < 0){
			printk("Something went wrong when reading from SD card. Error nr. %d\n", ret);
			return ret;
		}
		size_t pcm_data_buf_size = 1920;
		ret = sw_codec_lc3_dec_run(lc3_frame, lc3_frame_length,
					   pcm_data_buf_size, 0, pcm_mono_frame,
					   &pcm_mono_write_size, false);
		if (ret < 0) {
			printk("Error when running decoder. Error nr. %d\n", ret);
			return ret;
		}
		/* Convert from mono to stereo */
		ret = pscm_zero_pad(pcm_mono_frame, pcm_mono_write_size, 0, AUDIO_LC3_BIT_DEPTH, pcm_stereo_frame, &pcm_stereo_write_size);
		if (ret < 0) {
			printk("Error when converting to stereo. Error nr. %d\n", ret);
			return ret;
		}
		k_sem_take(&m_sem_load_from_buf_lc3, K_FOREVER);
		audio_lc3_buffer_to_ringbuffer(pcm_stereo_frame, pcm_stereo_write_size);
	}
	sd_card_segment_close();
	return 0;
}

int audio_lc3_sw_codec_decoder_init(uint16_t frame_duration_ms, uint32_t sample_rate,
				    uint8_t bit_depth, uint8_t channels)
{
	int ret;
	uint16_t frame_duration_us = frame_duration_ms * 1000;

	ret = sw_codec_lc3_init(NULL, NULL, frame_duration_us);
	if (ret < 0) {
		printk("Sw codec failed to initialize. Error: %d", ret);
		return ret;
	}
	printk("Sw codec initialized successfully.\n");


	ret = sw_codec_lc3_dec_init(sample_rate, bit_depth, frame_duration_us, channels);
	if (ret < 0) {
		printk("Sw codec decoder failed to initialize. Error: %d", ret);
		return ret;
	}
	printk("Sw codec decoder initialized successfully.\n");

	return 0;
}

int audio_lc3_play_init(const char *filename, const char *path_to_file){
	int ret;

	ret = sd_card_segment_open(filename, path_to_file);
	if (ret < 0) {
		printk("Error when trying to open file on SD card. Return value: %d", ret);
		return ret;
	}

	/* Read the header */
	ret = sd_card_segment_read((char *)&header, &header_size);
	if (ret < 0) {
		printk("Error when trying to peek at file  segment on SD card. Return value: %d", ret);
		return ret;
	}

	/* Figure out what the frame length is */
	ret = sd_card_segment_read((char *)&lc3_frame_length, sizeof(lc3_frame_length));
	if (ret < 0){
		printk("Error when trying to peek at file  segment on SD card. Return value: %d", ret);
		return ret;
	}

	ret = sd_card_segment_close();
	if (ret < 0) {
		printk("Error when trying to close file on SD card. Return value: %d", ret);
		return ret;
	}
}