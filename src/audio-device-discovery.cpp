// SPDX-FileCopyrightText: 2021-2023 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audio-device-discovery.hpp"

#if defined(__APPLE__)
#else
//#define ALSA_PCM_NEW_HW_PARAMS_API
//#define ALSA_PCM_NEW_SW_PARAMS_API
 #include <alsa/asoundlib.h>
#endif

#include <map>
#include <cstring>

#define DEBUGPRINT(...) printf(__VA_ARGS__); puts("");

#if defined(__APPLE__)
#else
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
#endif

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
   #if defined(__APPLE__)
   #else
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

           #ifdef __MOD_DEVICES__
            if (cardName != nullptr && *cardName != '\0')
            {
                if (std::strcmp(cardName, "MOD DUO") == 0)
                    goto skip;
                if (std::strcmp(cardName, "MOD DUOX") == 0)
                    goto skip;
                if (std::strcmp(cardName, "MOD DWARF") == 0)
                    goto skip;
                if (std::strcmp(cardName, "USB Gadget") == 0)
                    goto skip;
                if (std::strcmp(cardName, "UAC2_Gadget") == 0)
                    goto skip;
            }
           #endif

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

   #ifdef __MOD_DEVICES__
    skip:
        snd_ctl_close(ctl);
   #endif
    }
   #endif

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

    bool ok = true;

   #if defined(__APPLE__)
   #else
    snd_pcm_info_t* info;
    snd_pcm_info_alloca(&info);

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
   #endif

    return ok;
}

void cleanup()
{
   #if defined(__APPLE__)
   #else
    snd_config_update_free_global();
   #endif
}
