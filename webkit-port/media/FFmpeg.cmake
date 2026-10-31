# ffmpeg media engine (USE_FFMPEG): software playback of progressive media
# and HLS through libavformat/libavcodec. The sources in
# platform/graphics/ffmpeg are maintained in ps5-browser's
# webkit-port/media/ffmpeg and copied here by webkit-port/media/sync-to-tree.sh.
list(APPEND WebCore_PRIVATE_INCLUDE_DIRECTORIES
    "${WEBCORE_DIR}/platform/graphics/ffmpeg"
)

list(APPEND WebCore_SOURCES
    platform/graphics/ffmpeg/AudioSink.cpp
    platform/graphics/ffmpeg/FFmpegAudioDecoder.cpp
    platform/graphics/ffmpeg/FFmpegDemuxer.cpp
    platform/graphics/ffmpeg/FFmpegResourceIO.cpp
    platform/graphics/ffmpeg/FFmpegVideoDecoder.cpp
    platform/graphics/ffmpeg/MediaPlayerPrivateFFmpeg.cpp
)

find_path(FFMPEG_INCLUDE_DIR NAMES libavformat/avformat.h)
if (NOT FFMPEG_INCLUDE_DIR)
    message(FATAL_ERROR "USE_FFMPEG: libavformat/avformat.h not found")
endif ()
list(APPEND WebCore_SYSTEM_INCLUDE_DIRECTORIES ${FFMPEG_INCLUDE_DIR})

# The payload SDK's ffmpeg 7.0 is static; these are its own libraries plus
# what `pkg-config --static --libs libavformat libavcodec libswscale
# libswresample libavutil` lists (x264 makes the combination GPL).
# pkg_check_modules() is a no-op in OptionsPlayStation.cmake, hence
# find_library(). OpenSSL is the same SDK archive WebCore already links for
# curl.
foreach (_ffmpeg_lib IN ITEMS avformat avcodec swscale swresample avutil x264 bz2 lzma iconv z)
    string(TOUPPER "${_ffmpeg_lib}" _ffmpeg_var)
    find_library(FFMPEG_${_ffmpeg_var}_LIBRARY NAMES ${_ffmpeg_lib})
    if (NOT FFMPEG_${_ffmpeg_var}_LIBRARY)
        message(FATAL_ERROR "USE_FFMPEG: lib${_ffmpeg_lib} not found")
    endif ()
    list(APPEND WebCore_LIBRARIES ${FFMPEG_${_ffmpeg_var}_LIBRARY})
endforeach ()
list(APPEND WebCore_LIBRARIES OpenSSL::SSL OpenSSL::Crypto Threads::Threads m)

# Audio output through SDL2 (the PS5 port's sceAudioOut driver) when the
# SDK provides it; otherwise the engine plays silently (NullAudioSink).
find_path(FFMPEG_SDL2_INCLUDE_DIR NAMES SDL2/SDL.h)
find_library(FFMPEG_SDL2_LIBRARY NAMES SDL2)
if (FFMPEG_SDL2_INCLUDE_DIR AND FFMPEG_SDL2_LIBRARY)
    list(APPEND WebCore_PRIVATE_DEFINITIONS FFMPEG_MEDIA_SDL_AUDIO=1)
    list(APPEND WebCore_SYSTEM_INCLUDE_DIRECTORIES ${FFMPEG_SDL2_INCLUDE_DIR})
    list(APPEND WebCore_LIBRARIES ${FFMPEG_SDL2_LIBRARY})
    message(STATUS "USE_FFMPEG: SDL2 audio output enabled")
else ()
    message(STATUS "USE_FFMPEG: SDL2 not found, audio output disabled")
endif ()
