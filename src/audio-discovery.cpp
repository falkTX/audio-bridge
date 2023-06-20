// SPDX-FileCopyrightText: 2021-2023 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audio-discovery.hpp"

//#define ALSA_PCM_NEW_HW_PARAMS_API
//#define ALSA_PCM_NEW_SW_PARAMS_API
#include <alsa/asoundlib.h>
#include <map>

#define DEBUGPRINT(...) printf(__VA_ARGS__); puts("");

static int nextPowerOfTwo(int size) noexcept
{
    // http://graphics.stanford.edu/~seander/bithacks.html#RoundUpPowerOf2
    --size;
    size |= size >> 1;
    size |= size >> 2;
    size |= size >> 4;
    size |= size >> 8;
    size |= size >> 16;
    return ++size;
}

static bool fillDeviceProperties(snd_pcm_t* const pcm,
                                 const bool isOutput,
                                 const unsigned sampleRate,
                                 DeviceProperties& props)
{
    snd_pcm_hw_params_t* params;
    snd_pcm_hw_params_alloca(&params);

    if (snd_pcm_hw_params_any(pcm, params) >= 0)
    {
        if (snd_pcm_hw_params_test_rate(pcm, params, sampleRate, 0) != 0)
        {
            DEBUGPRINT("snd_pcm_hw_params_test_rate fail");
            return false;
        }

        int dir = 0;
        snd_pcm_uframes_t minSize = 0, maxSize = 0;
        snd_pcm_hw_params_get_period_size_min(params, &minSize, &dir);
        snd_pcm_hw_params_get_period_size_max(params, &maxSize, &dir);

        minSize = std::max(nextPowerOfTwo(minSize), 32);
        maxSize = std::min(maxSize, 8192LU);

        std::vector<unsigned> bufsizes;
        std::map<unsigned,bool> used;

        for (snd_pcm_uframes_t s = minSize; s <= maxSize; s = nextPowerOfTwo(s+1))
        {
            if (snd_pcm_hw_params_test_period_size(pcm, params, minSize, dir) == 0)
            {
                if (used.count(s) == 0)
                {
                    used[s] = true;
                    bufsizes.push_back(s);
                }
            }

            // do not go above 4096
            if (s == 4096)
                break;
        }

        if (bufsizes.empty())
        {
            DEBUGPRINT("bufsizes.empty() fail");
            return false;
        }

        if (props.bufsizes.empty())
        {
            DEBUGPRINT("props.bufsizes assign");
            props.bufsizes = bufsizes;
        }
        // FIXME
        else if (props.bufsizes != bufsizes)
        {
            DEBUGPRINT("props.bufsizes != bufsizes fail | %u %u", props.bufsizes[0], bufsizes[0]);
            return false;
        }

        unsigned maxChans, minChans;
        snd_pcm_hw_params_get_channels_max(params, &maxChans);
        snd_pcm_hw_params_get_channels_min(params, &minChans);

        // put some sane limits
        maxChans = std::min(maxChans, 32U);
        minChans = std::min(minChans, maxChans);

        if (isOutput)
        {
            props.minChansOut = minChans;
            props.maxChansOut = maxChans;
        }
        else
        {
            props.minChansIn = minChans;
            props.maxChansIn = maxChans;
        }

        return true;
    }
    else
    {
        DEBUGPRINT("snd_pcm_hw_params_any fail");
    }

    return false;
}

static bool isdigit(const char* const s)
{
    const size_t len = strlen(s);

    if (len == 0)
        return false;

    for (size_t i=0; i<len; ++i)
    {
        if (std::isdigit(s[i]))
            continue;
        return false;
    }

    return true;
}

bool enumerateSoundcards(std::vector<DeviceID>& inputs, std::vector<DeviceID>& outputs)
{
    snd_ctl_t* ctl = nullptr;
    snd_ctl_card_info_t* cardinfo = nullptr;
    snd_ctl_card_info_alloca(&cardinfo);

    int card = -1;
    char hwcard[32];
    char reserve[32];

    while (inputs.size() + outputs.size() <= 64)
    {
        if (snd_card_next(&card) != 0 || card < 0)
            break;

        snprintf(hwcard, sizeof(hwcard), "hw:%i", card);

        if (snd_ctl_open(&ctl, hwcard, SND_CTL_NONBLOCK) < 0)
            continue;

        if (snd_ctl_card_info(ctl, cardinfo) >= 0)
        {
            const char* cardId = snd_ctl_card_info_get_id(cardinfo);
            const char* cardName = snd_ctl_card_info_get_name(cardinfo);

            if (cardId == nullptr || ::isdigit(cardId))
            {
                snprintf(reserve, sizeof(reserve), "%i", card);
                cardId = reserve;
            }

            if (cardName == nullptr || *cardName == '\0')
                cardName = cardId;

            int device = -1;

            snd_pcm_info_t* pcminfo;
            snd_pcm_info_alloca(&pcminfo);

            for (;;)
            {
                if (snd_ctl_pcm_next_device(ctl, &device) < 0 || device < 0)
                    break;

                snd_pcm_info_set_device(pcminfo, device);

                for (int subDevice = 0, nbSubDevice = 1; subDevice < nbSubDevice; ++subDevice)
                {
                    snd_pcm_info_set_subdevice(pcminfo, subDevice);

                    snd_pcm_info_set_stream(pcminfo, SND_PCM_STREAM_CAPTURE);
                    const bool isInput = (snd_ctl_pcm_info(ctl, pcminfo) >= 0);

                    snd_pcm_info_set_stream(pcminfo, SND_PCM_STREAM_PLAYBACK);
                    const bool isOutput = (snd_ctl_pcm_info(ctl, pcminfo) >= 0);

                    if (! (isInput || isOutput))
                        continue;

                    if (nbSubDevice == 1)
                        nbSubDevice = snd_pcm_info_get_subdevices_count(pcminfo);

                    std::string strid(hwcard);
                    std::string strname(cardName);

                    strid += ",";
                    strid += std::to_string(device);

                    if (const char* const pcmName = snd_pcm_info_get_name(pcminfo))
                    {
                        if (pcmName[0] != '\0')
                        {
                            strname += ", ";
                            strname += pcmName;
                        }
                    }

                    if (nbSubDevice != 1)
                    {
                        strid += ",";
                        strid += std::to_string(subDevice);
                        strname += " {";
                        strname += snd_pcm_info_get_subdevice_name(pcminfo);
                        strname += "}";
                    }

                    if (isInput)
                        inputs.push_back({ strid, strname });

                    if (isOutput)
                        outputs.push_back({ strid, strname });
                }
            }
        }

        snd_ctl_close(ctl);
    }

    return inputs.size() + outputs.size() != 0;
}

bool getDeviceProperties(const std::string& deviceID,
                         const bool checkInput,
                         const bool checkOutput,
                         const unsigned sampleRate,
                         DeviceProperties& props)
{
    props.minChansOut = props.maxChansOut = props.minChansIn = props.maxChansIn = 0;
    props.bufsizes.clear();

    if (deviceID.empty())
        return false;

    snd_pcm_info_t* info;
    snd_pcm_info_alloca(&info);

    bool ok = true;

    if (checkOutput)
    {
        snd_pcm_t* pcm;

        if (snd_pcm_open(&pcm, deviceID.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) >= 0)
        {
            ok = fillDeviceProperties(pcm, true, sampleRate, props);
            snd_pcm_close(pcm);
        }
        else
        {
            ok = false;
            DEBUGPRINT("snd_pcm_open playback fail");
        }
    }

    if (ok && checkInput)
    {
        snd_pcm_t* pcm;

        if (snd_pcm_open(&pcm, deviceID.c_str(), SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK) >= 0)
        {
            ok = fillDeviceProperties(pcm, false, sampleRate, props);
            snd_pcm_close(pcm);
        }
        else
        {
            ok = false;
            DEBUGPRINT("snd_pcm_open capture fail");
        }
    }

    return ok;
}

void cleanup()
{
    snd_config_update_free_global();
}

bool initAudio(Test& t, const char* const deviceID, unsigned long bufsize, unsigned rate)
{
    if (snd_pcm_open(&t.ctl, deviceID, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) >= 0)
    {
        snd_pcm_hw_params_t* params;
        snd_pcm_hw_params_alloca(&params);

        if (snd_pcm_hw_params_any(t.ctl, params) < 0)
        {
            DEBUGPRINT("snd_pcm_hw_params_any fail");
            return false;
        }
        if (snd_pcm_hw_params_set_rate_resample(t.ctl, params, 0) != 0)
        {
            DEBUGPRINT("snd_pcm_hw_params_set_rate_resample fail");
            return false;
        }
        if (snd_pcm_hw_params_set_access(t.ctl, params, SND_PCM_ACCESS_MMAP_INTERLEAVED) != 0)
        {
            DEBUGPRINT("snd_pcm_hw_params_set_access fail");
            return false;
        }
        if (snd_pcm_hw_params_set_format(t.ctl, params, SND_PCM_FORMAT_S16_LE) != 0)
        {
            DEBUGPRINT("snd_pcm_hw_params_set_format fail");
            return false;
        }
        if (snd_pcm_hw_params_set_channels(t.ctl, params, 2) != 0)
        {
            DEBUGPRINT("snd_pcm_hw_params_set_channels fail");
            return false;
        }

        if (snd_pcm_hw_params_set_rate_near(t.ctl, params, &rate, 0) != 0)
        {
            DEBUGPRINT("snd_pcm_hw_params_set_rate_near fail");
            return false;
        }

        bufsize *= 2;
        if (snd_pcm_hw_params_set_buffer_size_near(t.ctl, params, &bufsize) != 0)
        {
            DEBUGPRINT("snd_pcm_hw_params_set_buffer_size_near fail");
            return false;
        }

        bufsize /= 2;
        if (snd_pcm_hw_params_set_period_size_near(t.ctl, params, &bufsize, 0) != 0)
        {
            DEBUGPRINT("snd_pcm_hw_params_set_period_size_near fail");
            return false;
        }
        int err;

        if ((err = snd_pcm_hw_params(t.ctl, params)) != 0)
        {
            DEBUGPRINT("snd_pcm_hw_params fail %s", snd_strerror(err));
            return false;
        }

        if ((err = snd_pcm_prepare(t.ctl)) != 0)
        {
            DEBUGPRINT("snd_pcm_prepare fail %s", snd_strerror(err));
            return false;
        }

        return true;
    }

    return false;
}

#include <cstring>

// TODO cleanup, see what is needed
static int xrun_recovery(snd_pcm_t *handle, int err)
{
    static int count = 0;
    // if ((count % 200) == 0)
    {
        count = 1;
        printf("stream recovery\n");
    }

    if (err == -EPIPE)
    {
        /* under-run */
        err = snd_pcm_prepare(handle);
        if (err < 0)
            printf("Can't recovery from underrun, prepare failed: %s\n", snd_strerror(err));
        return 0;
    }
    else if (err == -ESTRPIPE)
    {
        while ((err = snd_pcm_resume(handle)) == -EAGAIN)
            sleep(1);   /* wait until the suspend flag is released */

        if (err < 0)
        {
            err = snd_pcm_prepare(handle);
            if (err < 0)
                printf("Can't recovery from suspend, prepare failed: %s\n", snd_strerror(err));
        }

        return 0;
    }

    return err;
}

void runAudio(Test& t, unsigned frames)
{
    // std::memset(t.b1, 0, sizeof(float)*frames);
    // std::memset(t.b2, 0, sizeof(float)*frames);

    // const snd_pcm_state_t state = snd_pcm_state(t.ctl);
    //
    // static snd_pcm_state_t last_state = SND_PCM_STATE_PRIVATE1;
    // if (last_state != state)
    // {
    //     last_state = state;
    //     printf("alsa changed state %u\n", state);
    // }
    //
    // switch (state)
    // {
    // case SND_PCM_STATE_OPEN:
    // case SND_PCM_STATE_SETUP:
    //     break;
    // case SND_PCM_STATE_PREPARED:
    //     // printf("SND_PCM_STATE_PREPARED %d\n", snd_pcm_start(t.ctl));
    //     break;
    // case SND_PCM_STATE_RUNNING:
    // case SND_PCM_STATE_XRUN:
    // case SND_PCM_STATE_DRAINING:
    // case SND_PCM_STATE_PAUSED:
    // case SND_PCM_STATE_SUSPENDED:
    // case SND_PCM_STATE_DISCONNECTED:
    //     break;
    // }
    //
    // int avail = snd_pcm_avail_update(handle);

    // this assumes SND_PCM_ACCESS_MMAP_INTERLEAVED + SND_PCM_FORMAT_S16_LE
    int16_t rbuf[frames*2];
    for (unsigned i=0; i<frames; ++i)
    {
        rbuf[i*2+0] = std::min(32767.f, std::max(-32767.f, t.b1[i] * 32767));
        rbuf[i*2+1] = std::min(32767.f, std::max(-32767.f, t.b2[i] * 32767));
    }
    int16_t* ptr = rbuf;

    int err;

    while (frames > 0)
    {
        err = snd_pcm_mmap_writei(t.ctl, ptr, frames);

        if (err == -EAGAIN)
        {
            // DEBUGPRINT("err == -EAGAIN");
            continue;
        }

        if (err < 0)
        {
            if (xrun_recovery(t.ctl, err) < 0)
            {
                printf("Write error: %s\n", snd_strerror(err));
                exit(EXIT_FAILURE);
            }
            break;  /* skip one period */
        }
        ptr += err * 2;
        frames -= err;
    }
}
