# Audio-Bridge (for ALSA/JACK)

This project allows to bridge audio from a running [JACK](https://jackaudio.org/) instance into ALSA and vice-versa.  
It is similar to other projects such as `alsa_in/out` and `zita-a2j/j2a`, but those lacked the exact combination of factors needed to get some hardware running without big pauses or resyncs so this project was made.

It provides a regular JACK command-line tool, an internal JACK client and an LV2 plugin.  
Due to it targetting ALSA it will only run on Linux or systems where ALSA emulation is in place.

## Progress

A few things already work, while others are still in progress.

- [x] Audio card discovery
- [x] Capture and playback of 16/24/32-bit audio
- [x] Dynamic resampling for clock sync (WIP mostly works already)
- [x] JACK client for capture and playback (WIP stereo only)
- [x] LV2 plugin for capture and playback (WIP stereo only)
- [ ] Parse command-line input on JACK CLI tool
- [ ] Parse parameter input on internal JACK client
- [ ] Create barebones LV2 UI
- [ ] Report available devices to LV2 UI
- [ ] Dynamically switch devices in LV2 plugin
- [ ] Save and restore state in LV2 plugin
- [x] SIMD (SSE+NEON) optimized resampler

## Usage

For now the tool simply will try to connect to the last available soundcard in playback mode.  
A 1st optional argument can be given for choosing the soundcard, a 2nd one as "capture" for switching to capture mode.

Quickly building and running can be done like so:

```
cmake -S . -B build && cmake --build build && ./build/jack-audio-bridge hw:ALSA_HW_NAME
```

There is no way to specify amount of channels, so things are hardcoded to stereo at the moment.

The LV2 plugin will simply use the last available soundcard, without any controls or state.

## Support

There is no support whatsoever for this tool, if it works for you that's great, if not then go look elsewhere for alternative solutions or just stick with [PipeWire](https://pipewire.org/).

It is not meant to be a general user tool, just something that works for a few tested/verified use-cases.

## License

Copyright (C) 2021-2023 falkTX

This program is free software: you can redistribute it and/or modify it under the terms of the GNU Affero General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See [LICENSE] for the verbatim license.
