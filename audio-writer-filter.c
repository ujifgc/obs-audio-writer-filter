#include <obs-module.h>

#include "audio-writer-filter.h"
#include "media-io/audio-math.h"
#include "../UI/obs-frontend-api/obs-frontend-api.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("audio-writer-filter", "en-US")

#define S_FOLDER_PATH "folder_path"
#define TEXT_FOLDER_PATH obs_module_text("AudioWriterFilter.FolderPath")
#define S_OUTPUT_ENCODER "output_encoder"
#define TEXT_OUTPUT_ENCODER obs_module_text("AudioWriterFilter.OutputEncoder")

extern void write_wav_packet(writer_data_t *, struct obs_audio_data *);
extern void write_wav_placeholders(writer_data_t *);
extern void write_coreaudio_aac_packet(writer_data_t *, struct obs_audio_data *);
//extern void write_ffaac_packet(writer_data_t *, struct obs_audio_data *);
extern void write_raw_packet(writer_data_t *, struct obs_audio_data *);

/* Audio writer filter output formats */
encoder_t encoders[] = {
	{ "internal-wav",  "wav", write_wav_packet,           write_wav_placeholders },
	{ "coreaudio-aac", "aac", write_coreaudio_aac_packet, NULL },
//	{ "ffmpeg-aac",    "aac", write_ffaac_packet,         NULL },
	{ "internal-raw",  "raw", write_raw_packet,           NULL },
};

static encoder_t *get_encoder_by_name(const char *encoder_name) {
	for (int i = 0; i < sizeof(encoders) / sizeof(encoder_t); i++) {
		if (0 == strcmp(encoder_name, encoders[i].name)) return &encoders[i];
	}
	return &encoders[0];
}

#define OUTPUT_NAME_PREFIX "obs-audio-writer"
#define OUTPUT_DATE_FORMAT "%Y-%m-%d %H-%M-%S" // format 'yyyy-mm-dd hh-mm-ss'
#define PREFIX_SIZE sizeof(OUTPUT_NAME_PREFIX)
#define DATE_SIZE (sizeof(OUTPUT_DATE_FORMAT) + 2) // 2 for year hundreds
#define SUFFIX_SIZE (1 + PREFIX_SIZE + DATE_SIZE + 1) // 1 for dot, 1 for slash

static const char *new_output_filename(writer_data_t *data)
{
	if (data->output_folder == NULL || strlen(data->output_folder) == 0) return NULL;

	const char *parent_source_name = data->parent ? data->parent->context.name : "unknown";
	size_t new_filename_length =
		strlen(data->output_folder) +
		strlen(parent_source_name) +
		strlen(data->encoder->ext) +
		SUFFIX_SIZE + 1;
	data->output_filename = data->output_filename ?
		brealloc(data->output_filename, new_filename_length) :
		bzalloc(new_filename_length);

	time_t now = time(0);
	char formatted_time[DATE_SIZE];
	do {
		strftime(formatted_time, sizeof(formatted_time), OUTPUT_DATE_FORMAT, localtime(&now));
		now++;

		sprintf(data->output_filename, "%s/%s [%s] %s.%s",
			data->output_folder,
			OUTPUT_NAME_PREFIX,
			parent_source_name,
			formatted_time,
			data->encoder->ext);

		char *p = data->output_filename + strlen(data->output_folder) + 1;
		while (*p) {
			if (strchr("\\/:*?!&\"'<>|", *p)) *p = '_';
			p++;
		}
	} while (os_file_exists(data->output_filename));

	return data->output_filename;
}

bool open_output(writer_data_t *data)
{
	pthread_mutex_lock(&data->output_lock);
	if (data->file == NULL) {
		const char *new_filename = new_output_filename(data);
		data->file = new_filename ? fopen(new_filename, "wb") : NULL;
		data->data_length = 0;
		data->file_has_header = false;
	}
	pthread_mutex_unlock(&data->output_lock);

	return !!data->file;
}

void close_output(writer_data_t *data)
{
	pthread_mutex_lock(&data->output_lock);
	if (data->file != NULL) {
		if (data->encoder->write_finish) data->encoder->write_finish(data);
		fclose(data->file);
		data->file = NULL;
	}
	pthread_mutex_unlock(&data->output_lock);
}

static void writer_update(writer_data_t *data, obs_data_t *settings)
{
	data->output_folder = obs_data_get_string(settings, S_FOLDER_PATH);
	if (data->output_filename) {
		bool folder_changed = false;
		char *last_slash = strrchr(data->output_filename, '/');
		if (last_slash) {
			*last_slash = 0;
			folder_changed = strcmp(data->output_folder, data->output_filename);
			*last_slash = '/';
		}
		if (folder_changed)
			close_output(data);
	}

	const char *encoder_name = obs_data_get_string(settings, S_OUTPUT_ENCODER);
	encoder_t *new_encoder = get_encoder_by_name(encoder_name);
	if (new_encoder != data->encoder) {
		close_output(data);
		data->encoder = new_encoder;
	}
}

static const char *writer_get_name(writer_data_t *data)
{
	UNUSED_PARAMETER(data);

	return obs_module_text("Audio Writer");
}

static void frontend_event_callback(enum obs_frontend_event event, writer_data_t *data)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
		open_output(data);
		data->do_writing = true;
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STOPPING:
	case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
		data->do_writing = false;
		close_output(data);
		break;
	}
}

static void *writer_create(obs_data_t *settings, obs_source_t *filter)
{
	writer_data_t *data = (writer_data_t *)bzalloc(sizeof(writer_data_t));
	data->filter = filter;
	pthread_mutex_init(&data->output_lock, NULL);
	writer_update(data, settings);
	obs_frontend_add_event_callback(frontend_event_callback, data);
	return data;
}

static void writer_destroy(writer_data_t *data)
{
	close_output(data);

	if (data->output_filename != NULL) bfree(data->output_filename);

	circlebuf_free(&data->input_buffer);
	circlebuf_free(&data->encode_buffer);
	circlebuf_free(&data->output_buffer);
	circlebuf_free(&data->interleaved_buffer);

	pthread_mutex_destroy(&data->output_lock);
	bfree(data);
}

static struct obs_audio_data *writer_filter_audio(writer_data_t *data, struct obs_audio_data *audio)
{
	if (data->parent == NULL) {
		if (data->filter->filter_parent == NULL) return audio;
		data->parent = data->filter->filter_parent;
		data->sample_info = data->parent->sample_info;
	}

	if (data->do_writing) {
		data->encoder->write_packet(data, audio);
	}

	return audio;
}

static void writer_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, S_FOLDER_PATH, "");
	obs_data_set_default_string(settings, S_OUTPUT_ENCODER, encoders[0].name);
}

static obs_properties_t *writer_get_properties(writer_data_t *data)
{
	UNUSED_PARAMETER(data);

	obs_properties_t *properties = obs_properties_create();

	obs_properties_add_path(properties, S_FOLDER_PATH, TEXT_FOLDER_PATH, OBS_PATH_DIRECTORY, NULL, NULL);

	obs_property_t *property = obs_properties_add_list(properties, S_OUTPUT_ENCODER, TEXT_OUTPUT_ENCODER, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	for (int i = 0; i < sizeof(encoders) / sizeof(encoder_t); i++) {
		obs_property_list_add_string(property, encoders[i].name, encoders[i].name);
	}

	return properties;
}

bool obs_module_load(void)
{
	struct obs_source_info audio_writer_filter = {
		.id = "audio_writer_filter",
		.type = OBS_SOURCE_TYPE_FILTER,
		.output_flags = OBS_SOURCE_AUDIO,
		.get_name = writer_get_name,
		.create = writer_create,
		.destroy = writer_destroy,
		.update = writer_update,
		.filter_audio = writer_filter_audio,
		.get_defaults = writer_get_defaults,
		.get_properties = writer_get_properties,
	};
	obs_register_source(&audio_writer_filter);
	return true;
}
