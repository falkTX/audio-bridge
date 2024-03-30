# Audio-Bridge (for ALSA/JACK)

This project allows to bridge audio from a running [JACK](https://jackaudio.org/) instance into ALSA and vice-versa.  
It is similar to other projects such as `alsa_in/out` and `zita-a2j/j2a`, but those lacked the exact combination of factors needed to get some hardware running without big pauses or resyncs so this project was made.

Audio-Bridge provides a regular JACK command-line tool, an internal JACK client and an LV2 plugin.  
Due to it targetting ALSA it will only run on Linux or systems where ALSA emulation is in place.

## Usage

Audio-Bridge will simply try to connect to the last available soundcard in playback mode.  
For the JACK CLI variant a 1st optional argument can be given for choosing the soundcard, a 2nd one as "capture" for switching to capture mode.

Quickly building and running can be done like so:

```
cmake -S . -B build && cmake --build build && ./build/jack-audio-bridge hw:ALSA_HW_NAME playback
```

The JACK variants will wait until the specified soundcard is available an then register the client and ports,
so that the JACK port count can match the ALSA side.

The LV2 plugin is always stereo and will simply use the last available soundcard without any user-visible controls.  
Once it is saved in a DAW/Host it will keep that soundcard in the state for connecting to it again next time.

## Support

There is no support whatsoever for this tool, if it works for you that's great,
if not then go look elsewhere for alternative solutions or just stick with [PipeWire](https://pipewire.org/).

It is not meant to be a general user tool, just something that works for a few tested/verified use-cases.

## License

Copyright (C) 2021-2024 falkTX

This program is free software: you can redistribute it and/or modify it under the terms of the GNU Affero General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See [LICENSE](LICENSE) for the verbatim license.
