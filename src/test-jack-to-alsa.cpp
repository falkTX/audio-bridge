// SPDX-FileCopyrightText: 2021-2024 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#undef NDEBUG
#ifndef DEBUG
#define DEBUG 1
#endif

#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#include <alsa/asoundlib.h>
#include <jack/jack.h>
#include <unistd.h>

// SND_PCM_FORMAT_S32
// SND_PCM_FORMAT_S24_3LE
// SND_PCM_FORMAT_S24
// SND_PCM_FORMAT_S16
#define ALSA_FORMAT SND_PCM_FORMAT_S32
#define ALSA_NUM_PERIODS 3
#define ALSA_PERIOD_SIZE 1024
#define ALSA_SAMPLE_RATE 48000

#define NUM_CHANNELS 2

#if ALSA_FORMAT == SND_PCM_FORMAT_S32
typedef int32_t alsa_srcdata_t;
#else
#error TODO
#endif

// #define TEST_CAPTURE_MODE

struct ClientData {
    jack_client_t* client;
    jack_port_t* ports[NUM_CHANNELS];
    snd_pcm_t* pcm;
    std::atomic<bool> started;
};

// 0x7fffffff
static constexpr inline
int32_t float32(const double s)
{
    return s <= -1.f ? -2147483647 :
           s >= 1.f ? 2147483647 :
           std::lrint(s * 2147483647.f);
}

static int jack_process(const unsigned frames, void* const arg)
{
    assert(ALSA_PERIOD_SIZE == frames);

    ClientData* const d = static_cast<ClientData*>(arg);

    if (! atomic_load(&d->started))
        return 0;

    float* buffers[NUM_CHANNELS];
    for (int c = 0; c < NUM_CHANNELS; ++c)
        buffers[c] = static_cast<float*>(jack_port_get_buffer(d->ports[c], frames));

    alsa_srcdata_t data[ALSA_PERIOD_SIZE * NUM_CHANNELS];

   #ifdef TEST_CAPTURE_MODE
    assert(snd_pcm_mmap_readi(d->pcm, data, ALSA_PERIOD_SIZE) == ALSA_PERIOD_SIZE);

    for (int c = 0; c < NUM_CHANNELS; ++c)
    {
        for (int i = 0; i < ALSA_PERIOD_SIZE; ++i)
        {
           #if ALSA_FORMAT == SND_PCM_FORMAT_S32
            buffers[c][i] = static_cast<double>(data[i * NUM_CHANNELS + c]) * (1.0 / 2147483647.0);
           #else
            #error TODO
           #endif
        }
    }
   #else
    snd_pcm_state_t state = snd_pcm_state(d->pcm);
    assert(state == SND_PCM_STATE_RUNNING);

    snd_pcm_sframes_t avail = snd_pcm_avail_update(d->pcm);
    printf("avail %ld\n", avail);
    assert(avail >= ALSA_PERIOD_SIZE);

    for (int c = 0; c < NUM_CHANNELS; ++c)
    {
        for (int i = 0; i < ALSA_PERIOD_SIZE; ++i)
        {
           #if ALSA_FORMAT == SND_PCM_FORMAT_S32
            data[i * NUM_CHANNELS + c] = float32(buffers[c][i]);
           #else
            #error TODO
           #endif
        }
    }

    assert(snd_pcm_mmap_writei(d->pcm, data, ALSA_PERIOD_SIZE) == ALSA_PERIOD_SIZE);
   #endif

    return 0;
}

int main(int argc, const char* argv[])
{
    if (argc <= 1)
    {
        fprintf(stderr, "usage: %s device-id\n", argv[0]);
        return 1;
    }

   #ifdef __MOD_DEVICES__
    setenv("JACK_INTERNAL_CLIENT_SYNC", ".", 1);
   #endif

    ClientData d = {};
    int err;

    snd_pcm_hw_params_t* hwparams;
    snd_pcm_hw_params_alloca(&hwparams);

    snd_pcm_sw_params_t* swparams;
    snd_pcm_sw_params_alloca(&swparams);

   #ifdef TEST_CAPTURE_MODE
    constexpr int mode = SND_PCM_STREAM_CAPTURE;
   #else
    constexpr int mode = SND_PCM_STREAM_PLAYBACK;
   #endif
    constexpr int flags = SND_PCM_NONBLOCK
                        | SND_PCM_NO_AUTO_RESAMPLE
                        | SND_PCM_NO_AUTO_CHANNELS
                        | SND_PCM_NO_AUTO_FORMAT
                        | SND_PCM_NO_SOFTVOL;
    assert(snd_pcm_open(&d.pcm, argv[1], SND_PCM_STREAM_PLAYBACK, flags) == 0);
    assert(d.pcm != nullptr);

    assert(snd_pcm_hw_params_any(d.pcm, hwparams) == 0);
    assert(snd_pcm_hw_params_set_rate_resample(d.pcm, hwparams, 0) == 0);
    assert(snd_pcm_hw_params_set_access(d.pcm, hwparams, SND_PCM_ACCESS_MMAP_INTERLEAVED) == 0);
    assert(snd_pcm_hw_params_set_format(d.pcm, hwparams, ALSA_FORMAT) == 0);
    assert(snd_pcm_hw_params_set_rate(d.pcm, hwparams, ALSA_SAMPLE_RATE, 0) == 0);
    assert(snd_pcm_hw_params_set_periods(d.pcm, hwparams, ALSA_NUM_PERIODS, 0) == 0);
    assert(snd_pcm_hw_params_set_period_size(d.pcm, hwparams, ALSA_PERIOD_SIZE * ALSA_NUM_PERIODS, 0) == 0);
    assert(snd_pcm_hw_params_set_channels(d.pcm, hwparams, NUM_CHANNELS) == 0);
    assert(snd_pcm_hw_params(d.pcm, hwparams) == 0);

    assert(snd_pcm_sw_params_current(d.pcm, swparams) == 0);
    assert(snd_pcm_sw_params_set_tstamp_mode(d.pcm, swparams, SND_PCM_TSTAMP_MMAP) == 0);
    assert(snd_pcm_sw_params_set_tstamp_type(d.pcm, swparams, SND_PCM_TSTAMP_TYPE_MONOTONIC_RAW) == 0);
   #ifdef TEST_CAPTURE_MODE
    assert(snd_pcm_sw_params_set_avail_min(d.pcm, swparams, 0) == 0);
    assert(snd_pcm_sw_params_set_start_threshold(d.pcm, swparams, ALSA_PERIOD_SIZE * 2) == 0);
   #else
    // assert(snd_pcm_sw_params_set_avail_min(d.pcm, swparams, ALSA_PERIOD_SIZE * ALSA_NUM_PERIODS * 1.5) == 0);
    assert(snd_pcm_sw_params_set_start_threshold(d.pcm, swparams, 0) == 0);
   #endif
    assert(snd_pcm_sw_params_set_stop_threshold(d.pcm, swparams, (snd_pcm_uframes_t)-1) == 0);
    assert(snd_pcm_sw_params_set_silence_threshold(d.pcm, swparams, 0) == 0);
    assert(snd_pcm_sw_params(d.pcm, swparams) == 0);

    assert(snd_pcm_prepare(d.pcm) == 0);

    d.client = jack_client_open("audio-bridge-playback", JackNoStartServer, nullptr);
    assert(d.client);

    for (int i = 0; i < NUM_CHANNELS; ++i)
    {
        char name[16] = {};
        std::snprintf(name, sizeof(name) - 1, "p%d", i + 1);

       #ifdef TEST_CAPTURE_MODE
        constexpr int flags = JackPortIsOutput;
       #else
        constexpr int flags = JackPortIsInput;
       #endif
        d.ports[i] = jack_port_register(d.client, name, JACK_DEFAULT_AUDIO_TYPE, flags|JackPortIsTerminal, 0);
        assert(d.ports[i]);
    }

    jack_set_process_callback(d.client, jack_process, &d);
    jack_activate(d.client);

    assert(snd_pcm_start(d.pcm) == 0);

    // wait until alsa buffers are empty and then ready
    for (snd_pcm_sframes_t avail;;)
    {
        avail = snd_pcm_avail_update(d.pcm);
        printf("init avail %ld\n", avail);

        if (avail == 0)
            break;

        snd_pcm_forward(d.pcm, avail);
    }

    snd_pcm_wait(d.pcm, -1);

    snd_pcm_sframes_t avail = snd_pcm_avail_update(d.pcm);
    printf("init2 avail %ld\n", avail);

    atomic_store(&d.started, true);

    for (;;) { sleep(1); }

    jack_deactivate(d.client);
    jack_client_close(d.client);

    snd_pcm_close(d.pcm);

    return 0;
}
