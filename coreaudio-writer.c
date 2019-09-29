#include "audio-writer-filter.h"
#include "coreaudio-writer.h"

void *coreaudio_library = NULL;

#define LOAD_PROC(name) if (!(name = os_dlsym(coreaudio_library, #name))) failed = true;

inline bool load_core_audio()
{
	if (coreaudio_library) return true;
	if (coreaudio_library = os_dlopen("CoreAudioToolbox")) {
		bool failed = false;
		LOAD_PROC(AudioConverterNew);
		LOAD_PROC(AudioConverterDispose);
		LOAD_PROC(AudioConverterReset);
		LOAD_PROC(AudioConverterGetProperty);
		LOAD_PROC(AudioConverterGetPropertyInfo);
		LOAD_PROC(AudioConverterSetProperty);
		LOAD_PROC(AudioConverterFillComplexBuffer);
		LOAD_PROC(AudioFormatGetProperty);
		LOAD_PROC(AudioFormatGetPropertyInfo);
		if (failed) {
			os_dlclose(coreaudio_library);
			coreaudio_library = NULL;
		}
	}
	return !!coreaudio_library;
}

inline bool converter_create(writer_data_t *data)
{
	if (data->converter) return true;
	if (!load_core_audio()) return false;

	bool success = true;

	AudioStreamBasicDescription in = { 0 }, out = { 0 };

	in.mSampleRate = data->sample_info.samples_per_sec;
	in.mChannelsPerFrame = data->sample_info.speakers;
	in.mFormatID = kAudioFormatLinearPCM;
	in.mFormatFlags = kAudioFormatFlagIsPacked | kAudioFormatFlagIsFloat;
	in.mBytesPerFrame = (UInt32)get_audio_size(AUDIO_FORMAT_FLOAT, in.mChannelsPerFrame, 1);
	in.mFramesPerPacket = 1;
	in.mBytesPerPacket = in.mFramesPerPacket * in.mBytesPerFrame;
	in.mBitsPerChannel = BYTES_PER_SAMPLE * 8;

	data->bytes_per_input_packet = in.mBytesPerPacket;

	out.mSampleRate = in.mSampleRate;
	out.mChannelsPerFrame = in.mChannelsPerFrame;
	out.mFormatID = kAudioFormatMPEG4AAC;

	OSStatus code;
	UInt32 size = sizeof(out);

	code = AudioFormatGetProperty(kAudioFormatProperty_FormatInfo, 0, NULL, &size, &out);

	code = AudioConverterNew(&in, &out, (AudioConverterRef *)&data->converter);
	if (code) success = false;

	//UInt32 rate_control = kAudioCodecBitRateControlMode_Constant;
	UInt32 rate_control = kAudioCodecBitRateControlMode_Variable;
	code = AudioConverterSetProperty(data->converter, kAudioCodecPropertyBitRateControlMode, sizeof(rate_control), &rate_control);

	UInt32 converter_quality = kAudioConverterQuality_Max;
	code = AudioConverterSetProperty(data->converter, kAudioConverterCodecQuality, sizeof(converter_quality), &converter_quality);

	UInt32 bitrate = 320000;
	UInt32 quality = 127;

	switch (rate_control) {
	case kAudioCodecBitRateControlMode_Constant:
		code = AudioConverterSetProperty(data->converter, kAudioCodecPropertyCurrentTargetBitRate, sizeof(bitrate), &bitrate);
		break;
	case kAudioCodecBitRateControlMode_Variable:
		code = AudioConverterSetProperty(data->converter, kAudioCodecPropertySoundQualityForVBR, sizeof(quality), &quality);
		break;
	}

	UInt32 output_buffer_size;
	size = sizeof(output_buffer_size);
	code = AudioConverterGetProperty(data->converter, kAudioConverterPropertyMaximumOutputPacketSize, &size, &output_buffer_size);
	if (code) output_buffer_size = 32768;
	circlebuf_upsize(&data->output_buffer, output_buffer_size);

	return success;
}

inline uint8_t getSampleRateTableIndex(uint32_t sampleRate) {
	if (sampleRate == 48000) { return 3; }
	if (sampleRate == 44100) { return 4; }

	static const uint32_t kSampleRateTable[] = {
		96000, 88200, 64000, 48000, 44100, 32000,
		24000, 22050, 16000, 12000, 11025, 8000
	};
	const uint8_t tableSize = sizeof(kSampleRateTable) / sizeof(kSampleRateTable[0]);
	for (uint8_t index = 0; index < tableSize; index++) {
		if (sampleRate == kSampleRateTable[index]) return index;
	}

	return 4;
}

#define ADTS_PACKET_HEADER_LENGTH 7
#define OMX_AUDIO_AACObjectLC 2
/*
* ADTS (Audio data transport stream) header structure.
* It consists of 7 or 9 bytes (with or without CRC):
* 12 bits of syncword 0xFFF, all bits must be 1
* 1 bit of field ID. 0 for MPEG-4, and 1 for MPEG-2
* 2 bits of MPEG layer. If in MPEG-TS, set to 0
* 1 bit of protection absense. Set to 1 if no CRC.
* 2 bits of profile code. Set to 1 (The MPEG-4 Audio
*   object type minus 1. We are using AAC-LC = 2)
* 4 bits of sampling frequency index code (15 is not allowed)
* 1 bit of private stream. Set to 0.
* 3 bits of channel configuration code. 0 resevered for inband PCM
* 1 bit of originality. Set to 0.
* 1 bit of home. Set to 0.
* 1 bit of copyrighted steam. Set to 0.
* 1 bit of copyright start. Set to 0.
* 13 bits of frame length. It included 7 or 9 bytes header length.
*   it is set to (protection absense ? 7 : 9) + size(AAC frame)
* 11 bits of buffer fullness. 0x7FF for VBR.
* 2 bits of frames count in one packet. Set to 0.
*/
inline uint8_t *adts_packet_header(uint32_t packetLength, uint32_t mSampleRate, uint8_t mChannelCount) {
	static uint8_t header[ADTS_PACKET_HEADER_LENGTH];

	uint8_t data = 0xFF;
	header[0] = data;

	uint8_t kFieldId = 0;
	uint8_t kMpegLayer = 0;
	uint8_t kProtectionAbsense = 1;
	data = 0xF0;
	data |= (kFieldId << 3);
	data |= (kMpegLayer << 1);
	data |= kProtectionAbsense;
	header[1] = data;

	uint8_t kProfileCode = OMX_AUDIO_AACObjectLC - 1;
	uint8_t kSampleFreqIndex = getSampleRateTableIndex(mSampleRate);
	uint8_t kPrivateStream = 0;
	uint8_t kChannelConfigCode = mChannelCount;
	data = (kProfileCode << 6);
	data |= (kSampleFreqIndex << 2);
	data |= (kPrivateStream << 1);
	data |= (kChannelConfigCode >> 2);
	header[2] = data;

	// 4 bits from originality to copyright start
	uint8_t kCopyright = 0;
	uint32_t kFrameLength = ADTS_PACKET_HEADER_LENGTH + packetLength;
	data = ((kChannelConfigCode & 3) << 6);
	data |= (kCopyright << 2);
	data |= ((kFrameLength & 0x1800) >> 11);
	header[3] = data;

	data = (uint8_t)((kFrameLength & 0x07F8) >> 3);
	header[4] = data;

	uint32_t kBufferFullness = 0x7FF;  // VBR
	data = ((kFrameLength & 0x07) << 5);
	data |= ((kBufferFullness & 0x07C0) >> 6);
	header[5] = data;

	uint8_t kFrameCount = 0;
	data = ((kBufferFullness & 0x03F) << 2);
	data |= kFrameCount;
	header[6] = data;

	return header;
}

#define MORE_DATA_REQUIRED 1

static OSStatus input_data_provider(
	AudioConverterRef inAudioConverter,
	UInt32 *ioNumberDataPackets,
	AudioBufferList *ioData,
	AudioStreamPacketDescription **outDataPacketDescription,
	void *inUserData)
{
	UNUSED_PARAMETER(inAudioConverter);
	UNUSED_PARAMETER(outDataPacketDescription);

	writer_data_t *data = (writer_data_t *)inUserData;

	UInt32 bytes_required = (*ioNumberDataPackets) * data->bytes_per_input_packet;

	if (data->input_buffer.size < bytes_required) {
		*ioNumberDataPackets = 0;
		return MORE_DATA_REQUIRED;
	}

	circlebuf_upsize(&data->encode_buffer, bytes_required);

	ioData->mBuffers[0].mNumberChannels = (UInt32)data->sample_info.speakers;
	ioData->mBuffers[0].mDataByteSize = bytes_required;
	ioData->mBuffers[0].mData = circlebuf_data(&data->encode_buffer, 0);

	circlebuf_pop_front(&data->input_buffer, ioData->mBuffers[0].mData, bytes_required);

	return 0;
}

void write_coreaudio_aac_packet(writer_data_t *data, struct obs_audio_data *audio)
{
	if (!converter_create(data)) return;
	if (!open_output(data)) return;

	void *buffer = fill_interleaved_buffer(data, audio);
	circlebuf_push_back(&data->input_buffer, buffer, audio->frames * data->sample_info.speakers * BYTES_PER_SAMPLE);

	if (EBUSY == pthread_mutex_trylock(&data->output_lock)) {
		return;
	}
	else {
		UInt32 packets_count = 1;
		AudioBufferList output_buffers = { 0 };
		output_buffers.mNumberBuffers = 1;
		output_buffers.mBuffers[0].mNumberChannels = (UInt32)data->sample_info.speakers;
		output_buffers.mBuffers[0].mData = circlebuf_data(&data->output_buffer, 0);
		output_buffers.mBuffers[0].mDataByteSize = (UInt32)data->output_buffer.size;
		AudioConverterFillComplexBuffer(data->converter, input_data_provider, data, &packets_count, &output_buffers, NULL);

		if (packets_count > 0) {
			fwrite(
				adts_packet_header(
					output_buffers.mBuffers[0].mDataByteSize,
					data->sample_info.samples_per_sec,
					data->sample_info.speakers),
				ADTS_PACKET_HEADER_LENGTH, 1,
				data->file);
			fwrite(
				output_buffers.mBuffers[0].mData,
				output_buffers.mBuffers[0].mDataByteSize, 1,
				data->file);
		}

		pthread_mutex_unlock(&data->output_lock);
	}
}
