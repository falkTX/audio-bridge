#define DEBUG
#undef NDEBUG
#include <cassert>
#include <cstdio>

#include <alsa/asoundlib.h>

#define CHANNELS 2
#define SAMPLE_RATE 48000

#define CAPTURE_NUM_PERIODS 4
#define CAPTURE_PERIOD_SIZE 32

#define PLAYBACK_NUM_PERIODS 2
#define PLAYBACK_PERIOD_SIZE 8

void setup_capture(snd_pcm_t* const pcm)
{
    int err;

    snd_pcm_hw_params_t* params;
    snd_pcm_hw_params_alloca(&params);

    snd_pcm_sw_params_t* swparams;
    snd_pcm_sw_params_alloca(&swparams);

    err = snd_pcm_hw_params_any(pcm, params);
    assert(err == 0);

    err = snd_pcm_hw_params_set_rate_resample(pcm, params, 0);
    assert(err == 0);

    err = snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_MMAP_INTERLEAVED);
    assert(err == 0);

    err = snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_S32);
    assert(err == 0);

    err = snd_pcm_hw_params_set_rate(pcm, params, SAMPLE_RATE, 0);
    assert(err == 0);

    err = snd_pcm_hw_params_set_periods(pcm, params, CAPTURE_NUM_PERIODS, 0);
    assert(err == 0);

    err = snd_pcm_hw_params_set_period_size(pcm, params, CAPTURE_PERIOD_SIZE, 0);
    assert(err == 0);

    err = snd_pcm_hw_params_set_channels(pcm, params, CHANNELS);
    assert(err == 0);

    err = snd_pcm_hw_params(pcm, params);
    assert(err == 0);

    err = snd_pcm_sw_params_current(pcm, swparams);
    assert(err == 0);

    // does nothing??
    err = snd_pcm_sw_params_set_avail_min(pcm, swparams, 1);
    assert(err == 0);

    // how many samples we need to write until audio hw starts ??
    err = snd_pcm_sw_params_set_start_threshold(pcm, swparams, 0);
    assert(err == 0);

    err = snd_pcm_sw_params_set_stop_threshold(pcm, swparams, (snd_pcm_uframes_t)-1);
    assert(err == 0);

    err = snd_pcm_sw_params_set_silence_threshold(pcm, swparams, 0);
    assert(err == 0);

    err = snd_pcm_sw_params(pcm, swparams);
    assert(err == 0);
}

void setup_playback(snd_pcm_t* const pcm)
{
    int err;

    snd_pcm_hw_params_t* params;
    snd_pcm_hw_params_alloca(&params);

    snd_pcm_sw_params_t* swparams;
    snd_pcm_sw_params_alloca(&swparams);

    err = snd_pcm_hw_params_any(pcm, params);
    assert(err == 0);

    err = snd_pcm_hw_params_set_rate_resample(pcm, params, 0);
    assert(err == 0);

    err = snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_MMAP_INTERLEAVED);
    assert(err == 0);

    err = snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_S32);
    assert(err == 0);

    err = snd_pcm_hw_params_set_rate(pcm, params, SAMPLE_RATE, 0);
    assert(err == 0);

    err = snd_pcm_hw_params_set_periods(pcm, params, PLAYBACK_NUM_PERIODS, 0);
    assert(err == 0);

    err = snd_pcm_hw_params_set_period_size(pcm, params, PLAYBACK_PERIOD_SIZE, 0);
    assert(err == 0);

    err = snd_pcm_hw_params_set_channels(pcm, params, CHANNELS);
    assert(err == 0);

    err = snd_pcm_hw_params(pcm, params);
    assert(err == 0);

    err = snd_pcm_sw_params_current(pcm, swparams);
    assert(err == 0);

    // unused in playback?
    err = snd_pcm_sw_params_set_avail_min(pcm, swparams, 0);
    assert(err == 0);

    // how many samples we need to write until audio hw starts, does nothing??
    err = snd_pcm_sw_params_set_start_threshold(pcm, swparams, 0);
    assert(err == 0);

    err = snd_pcm_sw_params_set_stop_threshold(pcm, swparams, (snd_pcm_uframes_t)-1);
    assert(err == 0);

    err = snd_pcm_sw_params_set_silence_threshold(pcm, swparams, 0);
    assert(err == 0);

    err = snd_pcm_sw_params(pcm, swparams);
    assert(err == 0);
}

int main()
{
    FILE* const f = fopen("out.raw", "r");
    assert(f);

    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    assert(size != 0);

    fseek(f, 0, SEEK_SET);

    int32_t* const data = (int32_t*)malloc(size);
    const size_t r = fread(data, 1, size, f);
    assert(r == size);

    fclose(f);

    int32_t* const capdata1 = (int32_t*)malloc(CHANNELS * PLAYBACK_PERIOD_SIZE * sizeof(int32_t) * 3);
//     int32_t* const capdata2 = (int32_t*)malloc(CHANNELS * CAPTURE_PERIOD_SIZE * sizeof(int32_t) * 2);

    snd_pcm_t* pcmCapture;
    snd_pcm_t* pcmPlayback;
    int err;

    const int flags = SND_PCM_NO_AUTO_RESAMPLE | SND_PCM_NO_AUTO_CHANNELS | SND_PCM_NO_AUTO_FORMAT | SND_PCM_NO_SOFTVOL;
#ifdef __aarch64__
    err = snd_pcm_open(&pcmCapture, "hw:UAC2Gadget", SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK | flags);
    assert(err >= 0);

    // SND_PCM_NONBLOCK | 
    err = snd_pcm_open(&pcmPlayback, "hw:DWARF", SND_PCM_STREAM_PLAYBACK, flags);
    assert(err >= 0);
#else
    err = snd_pcm_open(&pcmCapture, "hw:Rubix22", SND_PCM_STREAM_CAPTURE, flags);
    assert(err >= 0);

    err = snd_pcm_open(&pcmPlayback, "hw:Rubix22", SND_PCM_STREAM_PLAYBACK, flags);
    assert(err >= 0);
#endif

    setup_capture(pcmCapture);
    setup_playback(pcmPlayback);

    err = snd_pcm_link(pcmCapture, pcmPlayback);
    assert(err == 0);

    err = snd_pcm_prepare(pcmCapture);
    assert(err == 0);

    err = snd_pcm_prepare(pcmPlayback);
    assert(err == 0);

    assert(snd_pcm_poll_descriptors_count(pcmCapture) == 1);
    assert(snd_pcm_poll_descriptors_count(pcmPlayback) == 1);

    struct pollfd pfds[2] = {};

    err = snd_pcm_poll_descriptors(pcmCapture, &pfds[0], 1);
    assert(err == 1);

    err = snd_pcm_poll_descriptors(pcmPlayback, &pfds[1], 1);
    assert(err == 1);

    int32_t* const capdata = capdata1;
    int32_t* const playdata = capdata1;

    memset(capdata, 0, CHANNELS * PLAYBACK_PERIOD_SIZE * sizeof(int32_t));

//     err = snd_pcm_mmap_readi(pcmCapture, capdata1, CAPTURE_PERIOD_SIZE);
//     assert(err == CAPTURE_PERIOD_SIZE);

//     err = snd_pcm_mmap_readi(pcmCapture, capdata2, CAPTURE_PERIOD_SIZE);
//     assert(err == CAPTURE_PERIOD_SIZE);

//     err = snd_pcm_mmap_readi(pcmCapture, capdata1 + CAPTURE_PERIOD_SIZE, CAPTURE_PERIOD_SIZE);
//     assert(err == CAPTURE_PERIOD_SIZE);

//     for (int j = 0; j < PLAYBACK_PERIOD_SIZE;)
//     {
//         err = snd_pcm_mmap_readi(pcmCapture, capdata + j, CAPTURE_PERIOD_SIZE);
//         if (err == -EAGAIN)
//             continue;
//         assert(err > 0);
//         j += err;
//         fprintf(stderr, "wrote %d samples\n", j);
//         assert(j <= PLAYBACK_PERIOD_SIZE);
//     }

    // while(snd_pcm_wait(pcmCapture, -1) == 0) {}

//     pfds->fd = snd_pcm_file_descriptor (pcm, is_capture);
//     pfds->events = POLLHUP|POLLNVAL;
//     pfds->events |= (is_capture == SND_PCM_STREAM_PLAYBACK) ? POLLOUT : POLLIN;

    err = snd_pcm_start(pcmCapture);
    assert(err == 0);

//     err = snd_pcm_start(pcmPlayback);
//     assert(err == 0);

//     int capoff = 0;
    for (long i = 0; i < size;)
//     for (long i = 0;;)
    {
//         err = poll(pfds, 2, -1);
//         err = snd_pcm_wait(pcmCapture, -1);
//         assert(err == 1);
        err = snd_pcm_wait(pcmPlayback, -1);
        assert(err == 1);

//         err = snd_pcm_mmap_writei(pcmPlayback, playdata, PLAYBACK_PERIOD_SIZE);
//         assert(err == PLAYBACK_PERIOD_SIZE);

//         err = snd_pcm_nonblock(pcmPlayback, 1);
//         assert(err == 0);

//         while ((err = snd_pcm_mmap_writei(pcmPlayback, data + (i * CHANNELS), PLAYBACK_PERIOD_SIZE)) == -EAGAIN) {}
//             if (err == -EAGAIN)
//                 continue;
        err = snd_pcm_mmap_writei(pcmPlayback, data + (i * CHANNELS), PLAYBACK_PERIOD_SIZE);
//         fprintf(stderr, "wrote %d samples\n", err);
        assert(err == PLAYBACK_PERIOD_SIZE);

//         err = snd_pcm_mmap_readi(pcmCapture, capdata, CAPTURE_PERIOD_SIZE);
//         fprintf(stderr, "read %d samples\n", err);
//         assert(err == CAPTURE_PERIOD_SIZE);

//         err = snd_pcm_mmap_readi(pcmCapture, capdata + CAPTURE_PERIOD_SIZE, CAPTURE_PERIOD_SIZE);
//         fprintf(stderr, "read %d samples\n", err);
//         assert(err == CAPTURE_PERIOD_SIZE);

//         if (capoff != 0)
//         {
//             memmove(capdata, capdata + (PLAYBACK_PERIOD_SIZE - capoff), capoff * CHANNELS * sizeof(int32_t));
//         }

//         for (int j = capoff;;)
//         if (i > 3)
        {
//             err = snd_pcm_mmap_readi(pcmCapture, capdata, CAPTURE_PERIOD_SIZE);
//             assert(err == CAPTURE_PERIOD_SIZE);
//             if (err == -EAGAIN)
//                 continue;
//             assert(err > 0);
//             j += err;
//         fprintf(stderr, "wrote %d samples\n", j);
//             if (j >= PLAYBACK_PERIOD_SIZE)
//             {
//                 capoff = j - PLAYBACK_PERIOD_SIZE;
//                 break;
//             }
        }

//         capdata = capdata == capdata1 ? capdata2 : capdata1;
//         playdata = playdata == capdata1 ? capdata2 : capdata1;

//         err = snd_pcm_mmap_readi(pcmCapture, capdata + CAPTURE_PERIOD_SIZE, CAPTURE_PERIOD_SIZE);
//         assert(err == CAPTURE_PERIOD_SIZE);

//         if (err == CAPTURE_PERIOD_SIZE / 2)
//         {
// //         fprintf(stderr, "wrote %d samples\n", err);
//             err = snd_pcm_mmap_readi(pcmCapture, capdata + CAPTURE_PERIOD_SIZE / 2, CAPTURE_PERIOD_SIZE / 2);
//             assert(err == 0 || err == -EAGAIN || err == CAPTURE_PERIOD_SIZE / 2);
//         }
//         else
//         {
//         fprintf(stderr, "wrote %d samples\n", err);
//             assert(err == 0 || err == -EAGAIN || err == CAPTURE_PERIOD_SIZE);
//         }

//         if (err == 0 || err == -EAGAIN)
//             continue;
//         if (err < 0)
//             break;

        if ((i % SAMPLE_RATE) == 0)
            fprintf(stderr, "written %ldm:%02lds\n", i / SAMPLE_RATE / 60, (i / SAMPLE_RATE) % 60);

        i += err;

//         err = snd_pcm_nonblock(pcmPlayback, 0);
//         assert(err == 0);
    }

    free(capdata1);
//     free(capdata2);
    free(data);

    snd_pcm_close(pcmCapture);
    snd_pcm_close(pcmPlayback);
    return 0;
}
