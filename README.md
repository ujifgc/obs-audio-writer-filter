# An audio writer filter for OBS

## Use cases

- You stream something with Noise gate and Compressor filters but you want to keep the original audio for postproduction with better quality tools

## Usage

1. Download a zip from https://github.com/ujifgc/obs-audio-writer-filter/releases
2. Unpack it to your OBS folder (may require privileges)
3. Run OBS
4. Add an "Audio Writer" filter to an audio source (MIC for example)
5. Select a folder with enough space
6. Select WAV or AAC encoder (AAC requires CoreAudio to be installed)
7. Run a stream or recording, ensure that the Volume Meter is alive
8. Stop the stream or recording
9. You will have files named like "obs-audio-writer [MIC] 2019-09-29 12-05-48.aac" in the specified folder

## AAC encoder

The filter uses CoreAudio encoder to write AAC format. To install it you may have to follow instructions here:

https://obsproject.com/forum/resources/obs-studio-enable-coreaudio-aac-encoder-windows.220/

