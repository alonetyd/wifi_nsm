// Implementation of the C wrapper around micro_mp3::Mp3Decoder.
#include "micro_mp3/mp3_c_api.h"
#include "micro_mp3/mp3_decoder.h"

#include <new>

struct micro_mp3_decoder {
    micro_mp3::Mp3Decoder impl;
};

extern "C" {

micro_mp3_decoder_t *micro_mp3_decoder_create(void)
{
    auto *d = new (std::nothrow) micro_mp3_decoder_t;
    if (!d) return nullptr;
    // Mp3Decoder constructor never throws and never allocates.
    return d;
}

void micro_mp3_decoder_destroy(micro_mp3_decoder_t *dec)
{
    if (dec) delete dec;
}

void micro_mp3_decoder_reset(micro_mp3_decoder_t *dec)
{
    if (dec) dec->impl.reset();
}

uint32_t micro_mp3_decoder_get_sample_rate(const micro_mp3_decoder_t *dec)
{
    return dec ? dec->impl.get_sample_rate() : 0;
}

uint8_t micro_mp3_decoder_get_channels(const micro_mp3_decoder_t *dec)
{
    return dec ? dec->impl.get_channels() : 0;
}

uint32_t micro_mp3_decoder_get_bitrate(const micro_mp3_decoder_t *dec)
{
    return dec ? dec->impl.get_bitrate() : 0;
}

uint32_t micro_mp3_decoder_get_samples_per_frame(const micro_mp3_decoder_t *dec)
{
    return dec ? dec->impl.get_samples_per_frame() : 0;
}

int micro_mp3_decoder_decode(micro_mp3_decoder_t *dec,
                              const uint8_t *input, size_t input_len,
                              int16_t *output, size_t output_frames,
                              size_t *bytes_consumed, size_t *frames_decoded)
{
    if (!dec || !input || !output || !bytes_consumed || !frames_decoded) {
        return -1;  // MP3_INPUT_INVALID
    }

    // The decoder outputs int16_t samples per channel. Worst case is 1152 samples
    // per channel * 2 channels = 4608 bytes (MP3_MIN_OUTPUT_BUFFER_BYTES).
    size_t output_bytes = output_frames * 2 * sizeof(int16_t);  // assume stereo worst case
    if (output_bytes < micro_mp3::MP3_MIN_OUTPUT_BUFFER_BYTES) {
        output_bytes = micro_mp3::MP3_MIN_OUTPUT_BUFFER_BYTES;
    }

    size_t consumed = 0, samples = 0;
    micro_mp3::Mp3Result r = dec->impl.decode(
        input, input_len,
        reinterpret_cast<uint8_t *>(output),
        output_bytes,
        consumed, samples);

    *bytes_consumed = consumed;
    *frames_decoded = samples;  // samples are per-channel, callers treat as frames

    return static_cast<int>(r);
}

}  // extern "C"