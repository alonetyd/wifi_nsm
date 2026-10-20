// C wrapper around the C++ micro_mp3::Mp3Decoder so that the existing
// audio_decoder.c (pure C) can call it without needing C++.
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration of the opaque handle. */
typedef struct micro_mp3_decoder micro_mp3_decoder_t;

/**
 * @brief  Allocate a new MP3 decoder (lazy initialization; cheap).
 */
micro_mp3_decoder_t *micro_mp3_decoder_create(void);

/**
 * @brief  Free an MP3 decoder and all of its internal state.
 */
void micro_mp3_decoder_destroy(micro_mp3_decoder_t *dec);

/**
 * @brief  Reset decoder state; next decode() call re-initializes.
 */
void micro_mp3_decoder_reset(micro_mp3_decoder_t *dec);

/**
 * @brief  Get the decoder's sample rate (Hz), 0 if no frame decoded yet.
 */
uint32_t micro_mp3_decoder_get_sample_rate(const micro_mp3_decoder_t *dec);

/**
 * @brief  Get the decoder's channel count (1 or 2), 0 if no frame decoded yet.
 */
uint8_t micro_mp3_decoder_get_channels(const micro_mp3_decoder_t *dec);

/**
 * @brief  Get current bitrate in kbps (0 if not yet known).
 */
uint32_t micro_mp3_decoder_get_bitrate(const micro_mp3_decoder_t *dec);

/**
 * @brief  Get MPEG samples per frame (1152 for MPEG1, 576 for MPEG2/2.5).
 */
uint32_t micro_mp3_decoder_get_samples_per_frame(const micro_mp3_decoder_t *dec);

/**
 * @brief  Decode one frame from the input buffer.
 *
 * @param  dec             Opaque decoder handle
 * @param  input           Pointer to MP3 data (must not be NULL)
 * @param  input_len       Number of bytes available at input
 * @param  output          Pointer to int16_t PCM output buffer
 * @param  output_frames   Capacity of output buffer in frames (stereo frames = samples-per-channel)
 * @param  bytes_consumed  [out] Bytes consumed from input this call
 * @param  frames_decoded  [out] Stereo frames (samples-per-channel) decoded this call
 *
 * @return  0 on success (frames_decoded > 0),
 *           1 if more data needed (partial frame buffered),
 *           2 if stream info just became ready (advance and call again),
 *          -4 if current frame is corrupt (skip & continue),
 *          -3 if output buffer is too small,
 *          -2 on allocation failure,
 *          -1 on invalid input, or other negative on fatal error.
 */
int micro_mp3_decoder_decode(micro_mp3_decoder_t *dec,
                              const uint8_t *input, size_t input_len,
                              int16_t *output, size_t output_frames,
                              size_t *bytes_consumed, size_t *frames_decoded);

#ifdef __cplusplus
}
#endif
