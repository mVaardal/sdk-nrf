#include "audio_wav.h"
#include "audio_i2s.h"
#include "sd_card.h"
#include "hw_codec.h"
#include <stdint.h>
#include <string.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>

#define DEFAULT_SOUND_VOLUME 100 // Ranges from 0 to 128
#define SD_CARD_MAX_FILENAME_LENGTH 32
#define SD_CARD_MAX_NUMBER_OF_FILES_IN_DIR 32
#define SD_CARD_MAX_PATH_LENGTH 32

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

int play_file_from_sd(const char *filename, char *path_to_file)
{
	printk("Opening file %s\n", filename);
	int ret = sd_card_segment_read_open(filename, path_to_file);
	if(ret < 0) return ret;

	// Start by filling the entire ringbuffer
	sd_card_to_buffer(SOUND_BUF_SIZE);

	// Start I2S transmission by setting up both buffersets
	ring_buf_get(&m_ringbuf_sound_data, (uint8_t *)m_i2s_tx_buf_a, I2S_16BIT_SAMPLE_NUM * 2);
	audio_i2s_start((const uint8_t *)m_i2s_tx_buf_a, (uint32_t *)m_i2s_rx_buf_a);
	ring_buf_get(&m_ringbuf_sound_data, (uint8_t *)m_i2s_tx_buf_b, I2S_16BIT_SAMPLE_NUM * 2);
	audio_i2s_set_next_buf((const uint8_t *)m_i2s_tx_buf_b, (uint32_t *)m_i2s_rx_buf_b);	

	hw_codec_volume_set(DEFAULT_SOUND_VOLUME);
	while(1) {
		// Wait for the load from SD semaphore to be set, signalling that the ringbuffer is half empty
		k_sem_take(&m_sem_load_from_sd, K_FOREVER);
		// Move data from the SD card to the buffer, one half at the time
		if (sd_card_to_buffer(SD_CARD_TRANSFER_SIZE) < SD_CARD_TRANSFER_SIZE) {
			// If the function returns less bytes than requested we have reached the end of the file. 
			// Exit the while loop and stop the I2S driver
			ret = hw_codec_volume_mute();
			if (ret < 0) return ret;
			break;
		}
	}

	audio_i2s_stop();

	printk("Closing file\n");

	return sd_card_segment_read_close();
}

void audio_wav_i2s_callback(uint32_t frame_start_ts, uint32_t *rx_buf_released, uint32_t const *tx_buf_released)
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


/* Shell functions */
static char sd_card_file_path[SD_CARD_MAX_PATH_LENGTH] = "";

static int cmd_play_wav_file(const struct shell *shell, size_t argc, char **argv)
{
	const char *filename = argv[1];
	int ret = play_file_from_sd(filename, sd_card_file_path);
	if (ret < 0) {
		shell_error(shell, "ERROR: Could not play audio from file: %s", filename);
		return ret;
	};
	return 0;
}


static int cmd_list_files(const struct shell *shell, size_t argc, char **argv)
{
	int ret;
	char buf[256] = {0};
	size_t buf_size = sizeof(buf);
	ret = sd_card_get_dir_overview(sd_card_file_path, buf, buf_size);
	if (ret < 0){
		return ret;
	}
	shell_print(shell, "%s", buf);
	return 0;
}

static int cmd_change_dir(const struct shell *shell, size_t argc, char **argv)
{
	if (argv[1][0] == '/'){ 
		sd_card_file_path[0] = '\0';
		shell_print(shell, "Current directory: root");
	} else{
		strcat(sd_card_file_path, &argv[1][0]);
		strcat(sd_card_file_path, "/");
		shell_print(shell, "Current directory: %s", sd_card_file_path);

	}
	return 0;
}

/* Creating subcommands (level 1 command) array for command "demo". */
SHELL_STATIC_SUBCMD_SET_CREATE(audio_sd_cmd,
			       SHELL_COND_CMD(CONFIG_SHELL, play_file, NULL, "Play file from SD card.",
					      cmd_play_wav_file),
			       SHELL_COND_CMD(CONFIG_SHELL, list_files, NULL, "List files and directories on SD card.",
					      cmd_list_files),
					SHELL_COND_CMD(CONFIG_SHELL, cd, NULL, "Change directory.",
					      cmd_change_dir),
			       SHELL_SUBCMD_SET_END);
/* Creating root (level 0) command "demo" without a handler */
SHELL_CMD_REGISTER(audio_sd, &audio_sd_cmd, "Play .wav-files", NULL);
