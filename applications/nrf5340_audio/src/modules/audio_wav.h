#include <stddef.h>
#include <stdint.h>
#ifndef _AUDIO_WAV_H_
#define _AUDIO_WAV_H_

/**@brief   Play audio from .wav-file on sd card. 
 * @param[in]   filename    Name of file
 * @param[in]   path_to_file    Path to file 
*/
int play_file_from_sd(const char *filename, char *path_to_file);

#endif