/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _SD_CARD_H_
#define _SD_CARD_H_

#include <stddef.h>
#include <zephyr/fs/fs.h>

/**@brief Print out the contents under SD card root path and write the content to buffer.
 *
 * @param[in]		path		Path of the folder which is going to be listed
 *					If assigned path is null, then listing the contents under
 *					root If assigned path doesn't exist, an error will be
 *					returned
 * @param[out]		buf		Buffer where data is written. If set to NULL, it will be
 *					ignored.
 * @param[in, out]	buf_size	Buffer size
 *
 * @retval		0 on success.
 *			-EPERM SD card read is ongoing somewhere else
 *			-ENODEV SD init failed. SD card likely not inserted
 *			-EINVAL Failed to append to buffer
 *			-FR_INVALID_NAME Path is too long
 *			Otherwise, error from underlying drivers
 */
int sd_card_list_files(const char *path, char *buf, size_t *buf_size);

/**@brief Write data from buffer into the file
 *
 * @note If the file already exists, data will be appended to the end of the file.
 *
 * @param[in]		filename	Name of the target file for writing, the default
 *					location is the root directoy of SD card, accept
					absolute path under root of SD card
 * @param[in]		data		which is going to be written into the file
 * @param[in, out]	size		Pointer to the number of bytes which is going to be written
 *					The actual written size will be returned
 *
 * @retval	0 on success.
 *		-ENODEV SD init failed. SD card likely not inserted
 *		Otherwise, error from underlying drivers
 */
int sd_card_open_write_close(char const *const filename, char const *const data, size_t *size);

/**@brief Read data from file into the buffer
 *
 * @param[in]		filename	Name of the target file for reading, the default location is
 *					the root directoy of SD card, accept absolute path under
 *					root of SD card
 * @param[out]		data		The buffer which will be filled by read file contents
 * @param[in, out]	size		Pointer to the number of bytes which wait to be read from
 *					the file The actual read size will be returned If the actual
 *					read size is 0, there will be a warning message which
					indicates that the file is empty
 *
 * @retval	0 on success.
 *		-ENODEV SD init failed. SD card likely not inserted
 *		Otherwise, error from underlying drivers
 */
int sd_card_open_read_close(char const *const filename, char *const data, size_t *size);

/**@brief	Open file on SD card
 *
 * param[in]	filename	Name of file to open
 *
 * @retval	0 on success
 *		-ENODEV SD init failed. SD likely not inserted
 *		-EPERM SD card read is ongoing somewhere else
 *		Otherwise, error from underlying drivers
 */
int sd_card_open(char const *const filename, struct fs_file_t *f_seg_read_entry);

/**@brief	Read segment on the open file on the SD card
 *
 * param[out]		data	Where the read data is stored
 * @param[in, out]	size	Number of bytes to be read from file
 *				The actual read size will be returned
 *				If the actual read size is 0, there will be a warning message
 *				which indicates that the file is empty
 *
 * @retval	0 on success
 *		-ENODEV SD init failed. SD likely not inserted
 *		Otherwise, error from underlying drivers
 */
int sd_card_read(char *const data, size_t *size, struct fs_file_t *f_seg_read_entry);

/**@brief	Close the file opened by the sd_card_segment_read_open function
 *
 * @retval	0 on success
 *		-EBUSY Segment read operation has not started
 *		Otherwise, error from underlying drivers
 */
int sd_card_close(struct fs_file_t *f_seg_read_entry);

/**@brief	Initialize the SD card interface and print out SD card details.
 *
 * @retval	0 on success
 *		-ENODEV SD init failed. SD card likely not inserted
 *		Otherwise, error from underlying drivers
 */
int sd_card_init(void);

#endif /* _SD_CARD_H_ */
