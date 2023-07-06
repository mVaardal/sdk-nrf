#include "audio_wav.h"
#include "audio_i2s.h"
#include "sd_card.h"
#include "hw_codec.h"
#include <stdint.h>
#include <string.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include "sw_codec_lc3.h"
#include "audio_datapath.h"

#define DEFAULT_SOUND_VOLUME 60 // Ranges from 0 to 128
#define SD_CARD_MAX_FILENAME_LENGTH 32
#define SD_CARD_MAX_NUMBER_OF_FILES_IN_DIR 32
#define SD_CARD_MAX_PATH_LENGTH 32

#define I2S_SAMPLES_BITS (I2S_SAMPLES_NUM)
#define I2S_16BIT_SAMPLE_NUM (I2S_SAMPLES_BITS*2)
#define I2S_BUF_BYTES		 (I2S_16BIT_SAMPLE_NUM * 2)
#define WAV_SOUND_BUF_SIZE (I2S_16BIT_SAMPLE_NUM * 2 * 4)
#define LC3_SOUND_BUF_SIZE (WAV_SOUND_BUF_SIZE/3)
#define SD_CARD_TRANSFER_SIZE (WAV_SOUND_BUF_SIZE / 2)

K_SEM_DEFINE(m_sem_load_from_sd, 0, 1);
K_SEM_DEFINE(m_sem_load_from_buf, 0, 1);
// RING_BUF_DECLARE(m_ringbuf_sound_data, WAV_SOUND_BUF_SIZE);
RING_BUF_DECLARE(m_ringbuf_sound_data, 3840);

static uint16_t m_i2s_tx_buf_a[I2S_16BIT_SAMPLE_NUM], m_i2s_rx_buf_a[I2S_16BIT_SAMPLE_NUM], m_i2s_tx_buf_b[I2S_16BIT_SAMPLE_NUM], m_i2s_rx_buf_b[I2S_16BIT_SAMPLE_NUM];

size_t sd_card_to_buffer(int numbytes)
{
	int ret;
	static uint8_t buf_ptr[LC3_SOUND_BUF_SIZE];
	size_t sd_read_length = LC3_SOUND_BUF_SIZE;
	uint8_t *pcm_data;
	uint16_t pcm_wr_size;
	size_t num_claimed_bytes;

	// Claim a buffer from the ringbuffer. This allows us to read the file data directly into
	// the buffer without requiring a memcpy
	num_claimed_bytes = ring_buf_put_claim(&m_ringbuf_sound_data, &pcm_data, numbytes);

	// For simplicity, assume the claim was successful (the flow of the program ensures this)
	// Read the data from the file and move it into the ringbuffer
	ret = sd_card_segment_read(buf_ptr, &sd_read_length);
	if (ret < 0){
		printk("Sd card seg read failed\n");
		__ASSERT_NO_MSG(ret == 0);
		return ret;
	}

	ret = sw_codec_lc3_dec_run(buf_ptr, sd_read_length,
			 num_claimed_bytes, 0, (uint16_t *)pcm_data,
				&pcm_wr_size, false);
	printk("Return value after decode function is: %d\n", ret);
	__ASSERT_NO_MSG(ret == 0);

	// Finish the claim, allowing the data to be read from the buffer in the I2S interrupt
	ring_buf_put_finish(&m_ringbuf_sound_data, sd_read_length);

	// Return the actual read length. When we reach end of file this will be lower than numbytes
	return sd_read_length;
}

int audio_wav_play_file_from_sd(const char *filename, char *path_to_file)
{
	int ret = sd_card_segment_open(filename, path_to_file);
	printk("Opening file %s\n", filename);
	if(ret < 0) {
		printk("Could not open file!\n");
		return ret;
	}

	// Start by filling the entire ringbuffer
	sd_card_to_buffer(WAV_SOUND_BUF_SIZE);

	// Start I2S transmission by setting up both buffersets
	ring_buf_get(&m_ringbuf_sound_data, (uint8_t *)m_i2s_tx_buf_a, I2S_BUF_BYTES);
	audio_i2s_start((const uint8_t *)m_i2s_tx_buf_a, (uint32_t *)m_i2s_rx_buf_a);
	ring_buf_get(&m_ringbuf_sound_data, (uint8_t *)m_i2s_tx_buf_b, I2S_BUF_BYTES);
	audio_i2s_set_next_buf((const uint8_t *)m_i2s_tx_buf_b, (uint32_t *)m_i2s_rx_buf_b);

	hw_codec_volume_set(DEFAULT_SOUND_VOLUME);
	while(1) {
		// Wait for the load from SD semaphore to be set, signalling that the ringbuffer is half empty
		k_sem_take(&m_sem_load_from_sd, K_FOREVER);
		// Move data from the SD card to the buffer, one half at the time
		int read_length = sd_card_to_buffer(SD_CARD_TRANSFER_SIZE);
		printk("Read length: %d\nSD CARD TRANSFER SIZE: %d\n", read_length, SD_CARD_TRANSFER_SIZE);
		if (read_length < SD_CARD_TRANSFER_SIZE) {
			printk("Done playing!!!\n");
			// If the function returns less bytes than requested we have reached the end of the file.
			// Exit the while loop and stop the I2S driver
			ret = hw_codec_volume_mute();
			if (ret < 0) return ret;
			break;
		}
	}

	audio_i2s_stop();

	printk("Closing file\n");

	return sd_card_segment_close();
}

int audio_lc3_play_file_from_sd(const char *filename, char *path_to_file)
{
	printk("Opening file %s\n", filename);
	int ret = sd_card_segment_open(filename, path_to_file);
	if (ret < 0) {
		printk("Could not open file!\n");
		return ret;
	}

	// Start by filling the entire ringbuffer
	sd_card_to_buffer(WAV_SOUND_BUF_SIZE);

	// Start I2S transmission by setting up both buffersets
	ring_buf_get(&m_ringbuf_sound_data, (uint8_t *)m_i2s_tx_buf_a, I2S_BUF_BYTES);
	audio_i2s_start((const uint8_t *)m_i2s_tx_buf_a, (uint32_t *)m_i2s_rx_buf_a);
	ring_buf_get(&m_ringbuf_sound_data, (uint8_t *)m_i2s_tx_buf_b, I2S_BUF_BYTES);
	audio_i2s_set_next_buf((const uint8_t *)m_i2s_tx_buf_b, (uint32_t *)m_i2s_rx_buf_b);

	hw_codec_volume_set(DEFAULT_SOUND_VOLUME);
	while (1) {
		// Wait for the load from SD semaphore to be set, signalling that the ringbuffer is half empty
		k_sem_take(&m_sem_load_from_sd, K_FOREVER);
		// Move data from the SD card to the buffer, one half at the time
		int read_length = sd_card_to_buffer(SD_CARD_TRANSFER_SIZE);
		printk("Read length: %d\nSD CARD TRANSFER SIZE: %d\n", read_length,
		       SD_CARD_TRANSFER_SIZE);
		if (read_length < SD_CARD_TRANSFER_SIZE) {
			printk("Done playing!!!\n");
			// If the function returns less bytes than requested we have reached the end of the file.
			// Exit the while loop and stop the I2S driver
			ret = hw_codec_volume_mute();
			if (ret < 0)
				return ret;
			break;
		}
	}

	audio_i2s_stop();

	printk("Closing file\n");

	return sd_card_segment_close();
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
	printk("leftover space in the ringbuffer: %d\n", ring_buf_space_get(&m_ringbuf_sound_data));
	if(ring_buf_space_get(&m_ringbuf_sound_data) > 1920) {
		k_sem_give(&m_sem_load_from_buf);
	}
}





/*LC3 Portion*/

typedef struct
{
  uint16_t fileId;            /* Constant value, 0xCC1C */
  uint16_t hdrSize;           /* Header size, 0x0012 */
  uint16_t sampleRate;        /* Sample frequency / 100 */
  uint16_t bitRate;           /* Bit rate / 100 (total for all channels) */
  uint16_t channels;          /* Number of channels */
  uint16_t frameMs;           /* Frame duration in ms * 100 */
  uint16_t rfu;               /* RFU value */
  uint16_t signalLen;         /* Number of samples in signal, 16 LSB */
  uint16_t signalLenRed;      /* Number of samples in signal, 16 MSB (>> 16) */
} lc3BinaryHdr_t;

typedef struct
{
  uint16_t frameLen;          /* Frame length in bytes (for all channels) */
  uint8_t  frameData[];       /* Frame data for all channels */
} lc3BinaryFrame_t;

typedef struct
{
  lc3BinaryHdr_t   hdr;       /* File header */
  lc3BinaryFrame_t frames[];  /* Frames */
} lc3BinaryFile_t;

int buffer_to_ringbuffer(uint8_t *buffer, size_t numbytes){
	static uint8_t *buf_ptr;

	numbytes = ring_buf_put_claim(&m_ringbuf_sound_data, &buf_ptr, numbytes);

	for (int i = 0; i<numbytes; i++){
		buf_ptr[i] = buffer[i];
	}
	ring_buf_put_finish(&m_ringbuf_sound_data, numbytes);
	return numbytes;
}

int audio_lc3_read_header_file(const char *filename, const char *filepath){
	printk("I2S_16BIT_SAMPLE_NUM: %d\n", I2S_16BIT_SAMPLE_NUM);
	printk("SD card transfer size: %d\n", SD_CARD_TRANSFER_SIZE);
	int ret;
	lc3BinaryHdr_t header = {0};
	size_t header_size = sizeof(header);
	uint16_t framelen1;
	uint16_t framelen;
	size_t framelen_size = 2;
	uint16_t pcm_data_stereo[960];
	size_t pcm_data_stereo_size;
	ret = sd_card_segment_open(filename, filepath);
	if (ret < 0) {
		printk("Could not open file!\n");
		return ret;
	}
	ret = sd_card_segment_read((char *)&header, &header_size);
	if (ret < 0){
		printk("Sd card seg read failed\n");
		__ASSERT_NO_MSG(ret == 0);
		return ret;
	}
	ret = sw_codec_lc3_init(NULL, NULL, header.frameMs * 10);
	if (ret < 0){
		printk("Sw codec failed to initialize. Error: %d", ret);
		return ret;
	}
	printk("Sw codec initialized successfully\n");
	ret = sw_codec_lc3_dec_init(header.sampleRate * 100,
						16,
						header.frameMs  * 10,
						header.channels);
	if (ret < 0){
		printk("Sw codec decoder failed to initialize. Error: %d", ret);
		return ret;
	}
	printk("Sw codec decoder initialized successfully\n");


	ret = sd_card_segment_read((char *)&framelen1, &framelen_size);
	if (ret < 0){
		printk("Sd card seg read failed\n");
		__ASSERT_NO_MSG(ret == 0);
		return ret;
	}
	printk("Segment read completed successfully\n");
	uint8_t buf[100];
	size_t buf_size1 = sizeof(buf);
	ret = sd_card_segment_read(buf, &buf_size1);
	if (ret < 0) return ret;

	uint16_t pcm_data[480];
	size_t pcm_data_buf_size = sizeof(pcm_data) * 2;
	uint16_t pcm_write_size;
	ret = sw_codec_lc3_dec_run(buf, buf_size1, pcm_data_buf_size, 0, pcm_data, &pcm_write_size, false);
	printk("Here are the parameters of sw_codec dec run: ---\n");
	printk("Buf size: %d\n", buf_size1);
	printk("Pcm data buf size: %d\n", pcm_data_buf_size);
	ret = pscm_zero_pad(pcm_data, 960,
						0, 16,
						pcm_data_stereo, &pcm_data_stereo_size);
	if (ret) {
		printk("something went wrong! 123\n");
		return ret;
	}
	printk("Size of pcm data buffer is: %d\n", sizeof(pcm_data));
	if (ret < 0) return ret;
	printk("SW decoder successfully run\n");
	// audio_lc3_play_from_buffer(pcm_data, pcm_data_buf_size);
	// start by filling the entire ringbuffer
	ret = buffer_to_ringbuffer((uint8_t *) pcm_data, pcm_write_size);
	if (ret < 0)return ret;

	// Start I2S transmission by setting up both buffersets
	ring_buf_get(&m_ringbuf_sound_data, (uint8_t *)m_i2s_tx_buf_a, I2S_BUF_BYTES);
	audio_i2s_start((const uint8_t *)m_i2s_tx_buf_a, (uint32_t *)m_i2s_rx_buf_a);
	ring_buf_get(&m_ringbuf_sound_data, (uint8_t *)m_i2s_tx_buf_b, I2S_BUF_BYTES);
	audio_i2s_set_next_buf((const uint8_t *)m_i2s_tx_buf_b, (uint32_t *)m_i2s_rx_buf_b);
	hw_codec_volume_set(DEFAULT_SOUND_VOLUME);

	while (1){
		ret = sd_card_segment_read((char *)&framelen, &framelen_size);
		// printk("The frame length is: %d\n", framelen);
		if (ret < 0) return ret;

		// printk("Waiting for semaphore\n");

		k_sem_take(&m_sem_load_from_buf, K_FOREVER);
		ret = sd_card_segment_read(buf, &buf_size1);
		if (ret < 0) return ret;
		ret = sw_codec_lc3_dec_run(buf, buf_size1, pcm_data_buf_size, 0, pcm_data, &pcm_write_size, false);
		if (ret < 0) return ret;
		// printk("SW decoder successfully run\n");
		ret = pscm_zero_pad(pcm_data, 960,
			0, 16,
			pcm_data_stereo, &pcm_data_stereo_size);
		if (ret) {
			return ret;
		}

		// printk("Pcm write size: %d\n", pcm_write_size);
		// printk("Pcm write size STEREO: %d\n", pcm_data_stereo_size);
		// printk("Done waiting for semaphore\n");
		printk("Amount of data sent over from buf to ringbuf: %d\n", pcm_data_stereo_size);
		ret = buffer_to_ringbuffer((uint8_t *)pcm_data_stereo, pcm_data_stereo_size);
		if (ret < 0){
			// printk("Error when calling buffer to ringbuffer function\n");
			return ret;
		}
	}

	ret = sd_card_segment_close();
	printk("\nFile ID: %d\n", header.fileId);
	printk("\nHeader size: %d\n", header.hdrSize);
	printk("\nSample rate: %d\n", header.sampleRate);
	printk("\nChannels: %d\n", header.channels);
	printk("\nframe Ms: %d\n", header.frameMs);
	printk("\nRFU value: %d\n", header.rfu);
	printk("\nSignal length: %d\n", header.signalLen + (header.signalLenRed<<16));
	// printk("\nNumber of samples in signal, 16 LSB  %d\n", header.signalLenRed);
	printk("\nFrame length1: %d\n", framelen1);
	printk("\nFrame length: %d\n", framelen);

	return 0;
}


/* Shell functions */
static char sd_card_file_path[SD_CARD_MAX_PATH_LENGTH] = "";

static int cmd_play_wav_file(const struct shell *shell, size_t argc, char **argv)
{
	const char *filename = argv[1];
	printk("Playing: %s%s\n", sd_card_file_path, filename);
	int ret = audio_wav_play_file_from_sd(filename, sd_card_file_path);
	if (ret < 0) {
		shell_error(shell, "ERROR: Could not play audio from file: %s", filename);
		return ret;
	};
	return 0;
}


static int cmd_list_files(const struct shell *shell, size_t argc, char **argv)
{
	int ret;
	char buf[254] = {0};
	printk("In cmd list files!\n");
	ret = sd_card_list_files(sd_card_file_path, buf, sizeof(buf));
	if (ret < 0){
		printk("Something went wrong!\n");
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
SHELL_STATIC_SUBCMD_SET_CREATE(audio_wav_cmd,
			       SHELL_COND_CMD(CONFIG_SHELL, play_file, NULL, "Play file from SD card.",
					      cmd_play_wav_file),
			       SHELL_COND_CMD(CONFIG_SHELL, list_files, NULL, "List files and directories on SD card.",
					      cmd_list_files),
					SHELL_COND_CMD(CONFIG_SHELL, cd, NULL, "Change directory.",
					      cmd_change_dir),
			       SHELL_SUBCMD_SET_END);
/* Creating root (level 0) command "demo" without a handler */
SHELL_CMD_REGISTER(audio_wav, &audio_wav_cmd, "Play .wav-files", NULL);
