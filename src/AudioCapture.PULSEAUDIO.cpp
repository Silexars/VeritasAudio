#include <Veritas/Audio/AudioCapture.h>

#include <pulse/pulseaudio.h>

using namespace Veritas;
using namespace Audio;

#include <iostream>

static void context_state_cb(pa_context* context, void* mainloop) { pa_threaded_mainloop_signal((pa_threaded_mainloop*) mainloop, 0); }
static void stream_state_cb(pa_stream *s, void *mainloop) { pa_threaded_mainloop_signal((pa_threaded_mainloop*) mainloop, 0); }
static void stream_success_cb(pa_stream *stream, int success, void *userdata) { return; }

static void stream_callback(pa_stream *stream, size_t bytes, void *userdata) {
    AudioCapture *capture = (AudioCapture*) userdata;

    uint8* data = 0;
    size_t size;
    pa_stream_peek((pa_stream*) stream, (const void**) &data, &size);

    capture->mtx.lock();
    assert(capture->buffer.write(data, size) == size);
    capture->mtx.unlock();

    pa_stream_drop((pa_stream*) stream);
}

AudioCapture::AudioCapture(uint32 framerate, uint8 channels, FORMAT iformat)
    : AudioNode(framerate, iformat)
    , AudioSource(framerate, iformat)
    , channels(channels)
    , framerate(framerate)
{
    setTimeSpan(getTimeSpan());
    pa_sample_format_t format = PA_SAMPLE_INVALID;
    switch (iformat) {
        case UINT8: format = PA_SAMPLE_U8; break;
        case INT16LE: format = PA_SAMPLE_S16LE; break;
        case INT32LE: format = PA_SAMPLE_S32LE; break;
        case INT16BE: format = PA_SAMPLE_S16BE; break;
        case INT32BE: format = PA_SAMPLE_S32BE; break;
        case FLOAT32LE: format = PA_SAMPLE_FLOAT32LE; break;
        case FLOAT32BE: format = PA_SAMPLE_FLOAT32BE; break;
        case FLOAT32: format = PA_SAMPLE_FLOAT32; break;
    }

    // Get a mainloop and its context
    pa_threaded_mainloop* mainloop = pa_threaded_mainloop_new();
    pa_mainloop_api *mainloop_api = pa_threaded_mainloop_get_api(mainloop);
    pa_context *context = pa_context_new(mainloop_api, "pcm-playback");

    // Set a callback so we can wait for the context to be ready
    pa_context_set_state_callback(context, &context_state_cb, mainloop);

    // Lock the mainloop so that it does not run and crash before the context is ready
    pa_threaded_mainloop_lock(mainloop);

    // Start the mainloop
    assert(pa_threaded_mainloop_start(mainloop) == 0);
    assert(pa_context_connect(context, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL) == 0);

    // Wait for the context to be ready
    for(;;) {
        pa_context_state_t context_state = pa_context_get_state(context);
        assert(PA_CONTEXT_IS_GOOD(context_state));
        if (context_state == PA_CONTEXT_READY) break;
        pa_threaded_mainloop_wait(mainloop);
    }

    // Create a playback stream
    pa_sample_spec sample_specifications;
    sample_specifications.format = format;
    sample_specifications.rate = framerate;
    sample_specifications.channels = channels;

    pa_channel_map map;
    pa_channel_map_init_auto(&map, channels, PA_CHANNEL_MAP_DEFAULT);

    pa_stream* stream = pa_stream_new(context, "Source", &sample_specifications, &map);
    pa_stream_set_state_callback(stream, stream_state_cb, mainloop);
    pa_stream_set_read_callback(stream, stream_callback, this);

    // recommended settings, i.e. server uses sensible values
    uint32 bytes = pa_usec_to_bytes(0.025 * 1000000, &sample_specifications);

    pa_buffer_attr buffer_attr;
    buffer_attr.maxlength = (uint32_t) -1;
    buffer_attr.prebuf = (uint32_t) -1;
    buffer_attr.minreq = (uint32_t) -1;
    buffer_attr.tlength = (uint32_t) -1;
    buffer_attr.fragsize = bytes;

    // Settings copied as per the chromium browser source
    pa_stream_flags_t stream_flags;
    stream_flags = (pa_stream_flags_t)(PA_STREAM_START_CORKED | PA_STREAM_INTERPOLATE_TIMING | PA_STREAM_NOT_MONOTONIC | PA_STREAM_AUTO_TIMING_UPDATE | PA_STREAM_ADJUST_LATENCY);

    // Connect stream to the default audio output sink
    assert(pa_stream_connect_record(stream, NULL, &buffer_attr, stream_flags) == 0);

    // Wait for the stream to be ready
    for(;;) {
        pa_stream_state_t stream_state = pa_stream_get_state(stream);
        assert(PA_STREAM_IS_GOOD(stream_state));
        if (stream_state == PA_STREAM_READY) break;
        pa_threaded_mainloop_wait(mainloop);
    }

    pa_threaded_mainloop_unlock(mainloop);

    // Uncork the stream so it will start playing
    pa_stream_cork(stream, 0, stream_success_cb, mainloop);
}

AudioCapture::~AudioCapture() {}

void AudioCapture::read(uint8 *buffer, uint32 bytes) {
    for (;;) {
        mtx.lock();
        if (this->buffer.getOccupied() > bytes) break;
        mtx.unlock();
    }

    this->buffer.read(buffer, bytes);
    mtx.unlock();
}

void AudioCapture::setTimeSpan(float32 timeSpan) {
    AudioNode::setTimeSpan(timeSpan);
    buffer.setSize(2.0f * timeSpan * getFramerate() * getBytesPerFrame());
}
