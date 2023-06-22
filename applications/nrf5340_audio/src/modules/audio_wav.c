#include "audio_wav.h"
#include "audio_i2s.h"
#include "hw_codec.h"
#include "sd_card.h"
#include <stdint.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/logging/log.h>


#define I2S_SAMPLES_BITS (I2S_SAMPLES_NUM)
#define I2S_16BIT_SAMPLE_NUM (I2S_SAMPLES_BITS*2)
#define I2S_BUF_BYTES		 (I2S_16BIT_SAMPLE_NUM * 2)
#define SOUND_BUF_SIZE (I2S_16BIT_SAMPLE_NUM * 2 * 4)
#define SD_CARD_TRANSFER_SIZE (SOUND_BUF_SIZE / 2)

K_SEM_DEFINE(m_sem_load_from_sd, 0, 1);
RING_BUF_DECLARE(m_ringbuf_sound_data, SOUND_BUF_SIZE);

static uint16_t m_i2s_tx_buf_a[I2S_16BIT_SAMPLE_NUM], m_i2s_rx_buf_a[I2S_16BIT_SAMPLE_NUM], m_i2s_tx_buf_b[I2S_16BIT_SAMPLE_NUM], m_i2s_rx_buf_b[I2S_16BIT_SAMPLE_NUM];

size_t sd_card_to_buffer(int numbytes)
{
	uint8_t *buf_ptr;
	size_t sd_read_length = numbytes;

	// Claim a buffer from the ringbuffer. This allows us to read the file data directly into 
	// the buffer without requiring a memcpy
	sd_read_length = ring_buf_put_claim(&m_ringbuf_sound_data, &buf_ptr, sd_read_length);

	// For simplicity, assume the claim was successful (the flow of the program ensures this)
	// Read the data from the file and move it into the ringbuffer
	sd_card_segment_read(buf_ptr, &sd_read_length);

	// Finish the claim, allowing the data to be read from the buffer in the I2S interrupt
	ring_buf_put_finish(&m_ringbuf_sound_data, sd_read_length);

	// Return the actual read length. When we reach end of file this will be lower than numbytes
	return sd_read_length;
}

int play_file_from_sd(const char *filename)
{
	printk("Start of play_file func\n");

	printk("Opening file %s\n", filename);
	int ret = sd_card_segment_read_open(filename);
	if(ret < 0) return ret;

	// Start by filling the entire ringbuffer
	sd_card_to_buffer(SOUND_BUF_SIZE);

	printk("play_file func 1\n");

	// Start I2S transmission by setting up both buffersets
	ring_buf_get(&m_ringbuf_sound_data, (uint8_t *)m_i2s_tx_buf_a, I2S_16BIT_SAMPLE_NUM * 2);
	printk("Start of play_file func 1.5\n");
	audio_i2s_start((const uint8_t *)m_i2s_tx_buf_a, (uint32_t *)m_i2s_rx_buf_a);
	printk("Start of play_file func 1.625\n");
	ring_buf_get(&m_ringbuf_sound_data, (uint8_t *)m_i2s_tx_buf_b, I2S_16BIT_SAMPLE_NUM * 2);
	printk("Start of play_file func 1.75\n");

	audio_i2s_set_next_buf((const uint8_t *)m_i2s_tx_buf_b, (uint32_t *)m_i2s_rx_buf_b);	
	printk("Start of play_file func 2\n");

	int count = 0;
	uint32_t last_uptime = k_uptime_get_32();
	uint32_t last_uptime_init = k_uptime_get_32();
	while(1) {
		// Wait for the load from SD semaphore to be set, signalling that the ringbuffer is half empty
		uint32_t current_uptime = k_uptime_get_32();
		uint32_t delta_uptime = current_uptime - last_uptime;
		last_uptime = current_uptime;
		printk("%d:\t%d\n", ++count, delta_uptime);

		k_sem_take(&m_sem_load_from_sd, K_FOREVER);
		printk("%d:\t%d\n", ++count, delta_uptime);

		
		// Move data from the SD card to the buffer, one half at the time
		if (sd_card_to_buffer(SD_CARD_TRANSFER_SIZE) < SD_CARD_TRANSFER_SIZE) {
			// If the function returns less bytes than requested we have reached the end of the file. 
			// Exit the while loop and stop the I2S driver
			ret = hw_codec_volume_mute();
			if (ret < 0) return ret;
			break;
		}
	}

	uint32_t time_usage = k_uptime_get_32()-last_uptime_init;


	printk("Stopping I2S\n");
	printk("Time usage:\t%d\n", time_usage);

	audio_i2s_stop();

	printk("Closing file\n");

	return sd_card_segment_read_close();
}

void i2s_callback(uint32_t frame_start_ts, uint32_t *rx_buf_released, uint32_t const *tx_buf_released)
{
	// Update the I2S buffers by reading from the ringbuffer
	if((uint16_t *)tx_buf_released == m_i2s_tx_buf_a) {
		ring_buf_get(&m_ringbuf_sound_data, (uint8_t *)m_i2s_tx_buf_a, I2S_BUF_BYTES);
		audio_i2s_set_next_buf((const uint8_t *)m_i2s_tx_buf_a, (uint32_t *)m_i2s_rx_buf_a);
	} else if((uint16_t *)tx_buf_released == m_i2s_tx_buf_b) {
		ring_buf_get(&m_ringbuf_sound_data, (uint8_t *)m_i2s_tx_buf_b, I2S_BUF_BYTES);
		audio_i2s_set_next_buf((const uint8_t *)m_i2s_tx_buf_b, (uint32_t *)m_i2s_rx_buf_b);
	} else {
		printk("Should not happen! 0x%x\n", (int)tx_buf_released);
		return;
	}

	// Check the current free space in the buffer. 
	// If more than half the buffer is free we should move more data from the SD card
	if(ring_buf_space_get(&m_ringbuf_sound_data) > SD_CARD_TRANSFER_SIZE) {
		k_sem_give(&m_sem_load_from_sd);
	}
}