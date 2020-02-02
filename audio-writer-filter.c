#include <obs-module.h>

#include "audio-writer-filter.h"
#include "media-io/audio-math.h"
#include "../UI/obs-frontend-api/obs-frontend-api.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("audio-writer-filter", "en-US")

#define S_FILENAME_FORMAT "filename_format"
#define TEXT_FILENAME_FORMAT obs_module_text("AudioWriterFilter.FilenameFormat")
#define DEFAULT_FILENAME_FORMAT "audio-writer-filter [%SRC] %CCYY-%MM-%DD %hh-%mm-%ss"
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

static const char *new_output_filename(writer_data_t *data)
{
	if (data->output_filename != NULL) bfree(data->output_filename);

	struct dstr temp = { 0 };
	dstr_init_copy(&temp, data->output_folder);
	dstr_cat_ch(&temp, '/');
	dstr_cat(&temp, data->output_filename_format);
	dstr_replace(&temp, "%SRC", data->parent ? data->parent->context.name : "unknown");
	data->output_filename = os_generate_formatted_filename(data->encoder->ext, true, temp.array);
	dstr_free(&temp);

	char *p = data->output_filename + strlen(data->output_folder) + 1;
	while (*p) {
		if (strchr("\\/:*?!&\"'<>|", *p)) *p = '_';
		p++;
	}

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
	data->output_filename_format = obs_data_get_string(settings, S_FILENAME_FORMAT);

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
		if (++data->writing_triggers_count > 0) open_output(data);
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STOPPING:
	case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
		if (--data->writing_triggers_count <= 0) close_output(data);
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
	obs_frontend_remove_event_callback(frontend_event_callback, data);

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

	if (data->writing_triggers_count > 0) {
		data->encoder->write_packet(data, audio);
	}

	return audio;
}

static const char *get_homedir()
{
	const char *home = getenv("HOME");
	if (home == NULL) home = getenv("USERPROFILE");
	if (home == NULL) home = "";
	return home;
}

static void writer_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, S_FOLDER_PATH, get_homedir());
	obs_data_set_default_string(settings, S_OUTPUT_ENCODER, encoders[0].name);
	obs_data_set_default_string(settings, S_FILENAME_FORMAT, DEFAULT_FILENAME_FORMAT);
}

static obs_properties_t *writer_get_properties(writer_data_t *data)
{
	obs_properties_t *properties = obs_properties_create();

	obs_properties_add_path(properties, S_FOLDER_PATH, TEXT_FOLDER_PATH, OBS_PATH_DIRECTORY, NULL, data->output_folder);
	obs_properties_add_text(properties, S_FILENAME_FORMAT, TEXT_FILENAME_FORMAT, OBS_TEXT_DEFAULT);

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
