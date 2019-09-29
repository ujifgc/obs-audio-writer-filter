#include "audio-writer-filter.h"

#pragma pack(push, 1)
// http://soundfile.sapp.org/doc/WaveFormat/
typedef struct {
	uint32_t ChunkID;
	uint32_t ChunkSize;
	uint32_t Format;
	uint32_t Subchunk1ID;
	uint32_t Subchunk1Size;
	uint16_t AudioFormat;
	uint16_t NumChannels;
	uint32_t SampleRate;
	uint32_t ByteRate;
	uint16_t BlockAlign;
	uint16_t BitsPerSample;
	uint32_t Subchunk2ID;
	uint32_t Subchunk2Size;
} wav_header_t;
#pragma pack(pop)

const int PLACEHOLDER1_OFFSET = 4;
const int PLACEHOLDER2_OFFSET = 40;
const int DATA_BEGINNING = 36; // 4 + (8 + 16) + (8)

/* has no sync, must be called inside locking mutex */
static inline void write_wav_header(writer_data_t *data)
{
	wav_header_t header = {
		*(uint32_t*)&"RIFF",
		0, // PLACEHOLDER1 for size of 2 chunks
		*(uint32_t*)&"WAVE",

		// chunk 1
		*(uint32_t*)&"fmt ",
		16, // size of fmt chunk
		BYTES_PER_SAMPLE - 1, // 1 PCM, 3 IEEE float
		(uint16_t)data->sample_info.speakers, // number of channels
		data->sample_info.samples_per_sec, // 48000 or 44100 in OBS
		data->sample_info.samples_per_sec * data->sample_info.speakers * BYTES_PER_SAMPLE, // bytes per second
		(uint16_t)data->sample_info.speakers * BYTES_PER_SAMPLE, // bytes per block including one sample of each channel
		8 * BYTES_PER_SAMPLE, // bits per sample

		// chunk 2
		*(uint32_t*)&"data",
		0 // PLACEHOLDER2 for chunk 2 size
	};

	fwrite(&header, sizeof(wav_header_t), 1, data->file);
	data->file_has_header = true;
}

void write_raw_packet(writer_data_t *data, struct obs_audio_data *audio)
{
	if (!open_output(data)) return;

	pthread_mutex_lock(&data->output_lock);

	void *buffer = fill_interleaved_buffer(data, audio);
	fwrite(buffer, BYTES_PER_SAMPLE, audio->frames * data->sample_info.speakers, data->file);

	pthread_mutex_unlock(&data->output_lock);
}

void write_wav_packet(writer_data_t *data, struct obs_audio_data *audio)
{
	uint32_t packet_length = BYTES_PER_SAMPLE * audio->frames * data->sample_info.speakers;

	if (data->data_length > UINT32_MAX - DATA_BEGINNING - packet_length) close_output(data);

	if (!open_output(data)) return;

	pthread_mutex_lock(&data->output_lock);

	if (!data->file_has_header) write_wav_header(data);
	
	void *buffer = fill_interleaved_buffer(data, audio);
	fwrite(buffer, packet_length, 1, data->file);
	data->data_length += packet_length;

	pthread_mutex_unlock(&data->output_lock);
}

/* has no sync, must be called inside locking mutex */
void write_wav_placeholders(writer_data_t *data)
{
	uint32_t chunks_length = DATA_BEGINNING + data->data_length;

	fseek(data->file, PLACEHOLDER1_OFFSET, SEEK_SET);
	fwrite(&chunks_length, sizeof(uint32_t), 1, data->file);

	fseek(data->file, PLACEHOLDER2_OFFSET, SEEK_SET);
	fwrite(&data->data_length, sizeof(uint32_t), 1, data->file);
}
