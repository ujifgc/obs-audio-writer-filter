#include "obs-internal.h"
#include "util/circlebuf.h"

#define BYTES_PER_SAMPLE 4 // always 4 as OBS uses AUDIO_FORMAT_FLOAT

typedef const struct {
	const char *name;
	const char *ext;
	void (*write_packet)(void*,void*);
	void (*write_finish)(void*);
} encoder_t;

typedef struct {
	obs_source_t *filter;
	obs_source_t *parent;
	int writing_triggers_count;
	struct resample_info sample_info;
	uint32_t bytes_per_input_packet;
	uint32_t bit_rate;
	
	void *converter;

	struct circlebuf input_buffer;
	struct circlebuf encode_buffer;
	struct circlebuf output_buffer;
	struct circlebuf interleaved_buffer;

	const char *output_folder;
	const char *output_ext;
	const char *output_filename_format;
	char *output_filename;
	encoder_t *encoder;

	FILE *file;
	bool file_has_header;
	uint32_t data_length;
	pthread_mutex_t output_lock;
} writer_data_t;

bool open_output(writer_data_t *data);
void close_output(writer_data_t *data);

static inline void *fill_interleaved_buffer(writer_data_t *data, struct obs_audio_data *audio)
{
	const size_t channels = data->sample_info.speakers;

	float **fdata = (float**)audio->data;

	circlebuf_upsize(&data->interleaved_buffer, channels * audio->frames * BYTES_PER_SAMPLE);
	float *buffer = circlebuf_data(&data->interleaved_buffer, 0);

	for (size_t c = 0; c < channels; c++) {
		if (fdata[c]) {
			for (size_t i = 0; i < audio->frames; i++) {
				buffer[i * channels + c] = fdata[c][i];
			}
		}
		else {
			for (size_t i = 0; i < audio->frames; i++) {
				buffer[i * channels + c] = 0.0f;
			}
		}
	}

	return buffer;
}
