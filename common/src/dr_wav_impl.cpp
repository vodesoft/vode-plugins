/* dr_wav implementation TU (public domain) plus a small C wrapper exposing
 * the subset of dr_wav used by vdplg::wavio. Compiled as C++ with extern "C"
 * guards around the dr_wav include (dr_wav.h has no extern "C" guards), so
 * other C++ TUs never include dr_wav.h directly. */
/* NOTE: pinned dr_wav is v0.14.6 — uses the modern pcm-frames API
 * (drwav_open_file_and_read_pcm_frames_f32 / drwav_write_pcm_frames), not
 * the legacy drwav_read_float / DRWAV_CONTAINER_RIFF names. */
#define DR_WAV_IMPLEMENTATION
extern "C" {
#include "dr_wav.h"
}

#include <string.h>
#include <stdint.h>

extern "C" {

int vdplg_drwav_load_float(const char* path, int* sampleRate, int* channels,
                           float* buffer, size_t bufferSize, size_t* framesOut)
{
	unsigned int ch = 0, sr = 0;
	drwav_uint64 totalFrames = 0;
	float* data = drwav_open_file_and_read_pcm_frames_f32(path, &ch, &sr, &totalFrames, NULL);
	if (data == NULL)
		return 0;

	size_t totalSamples = (size_t)totalFrames * (size_t)ch;
	if (totalSamples > bufferSize)
	{
		drwav_free(data, NULL);
		return 0;
	}
	memcpy(buffer, data, totalSamples * sizeof(float));
	drwav_free(data, NULL);

	*sampleRate = (int)sr;
	*channels = (int)ch;
	*framesOut = (size_t)totalFrames;
	return 1;
}

int vdplg_drwav_save_float(const char* path, int sampleRate, int channels,
                           const float* samples, size_t numSamples)
{
	drwav_data_format format;
	memset(&format, 0, sizeof(format));
	format.container = drwav_container_riff;
	format.format = DR_WAVE_FORMAT_IEEE_FLOAT;
	format.channels = (unsigned int)channels;
	format.sampleRate = (unsigned int)sampleRate;
	format.bitsPerSample = 32;

	drwav out;
	if (!drwav_init_file_write(&out, path, &format, NULL))
		return 0;

	drwav_uint64 framesWritten = drwav_write_pcm_frames(
	    &out, (drwav_uint64)(numSamples / (size_t)channels), samples);
	drwav_uninit(&out);
	return framesWritten == (drwav_uint64)(numSamples / (size_t)channels) ? 1 : 0;
}

} // extern "C"
