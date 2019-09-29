cmake_minimum_required(VERSION 3.5)

project(audio-writer-filter)

set(audio-writer-filter_HEADERS
	audio-writer-filter.h
	coreaudio-writer.h
)

set(audio-writer-filter_SOURCES
	audio-writer-filter.c
	coreaudio-writer.c
	internal-writer.c
)

add_library(audio-writer-filter MODULE
	${audio-writer-filter_SOURCES}
	${audio-writer-filter_HEADERS}
)

target_link_libraries(audio-writer-filter
	${frontend-tools_PLATFORM_LIBS}
	obs-frontend-api
	libobs
	w32-pthreads
)

install_obs_plugin_with_data(audio-writer-filter data)
