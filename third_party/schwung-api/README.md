# Schwung's plugin API headers (MIT)

From [charlesvestal/schwung](https://github.com/charlesvestal/schwung) at commit
`7175f155ad77e2dda8a20f9890e8525c1e349cbc`, `src/host/`, unchanged: `plugin_api_v1.h` (host_api_v1_t),
`audio_fx_api_v2.h` (`move_audio_fx_init_v2`), `midi_fx_api_v1.h` (`move_midi_fx_init`). MIT, Copyright (c) 2025-2026
Charles Vestal (LICENSE). RemoteFM14Move's src/fx.c hosts the user's installed Schwung MIDI FX and audio FX modules
through them. The test modules in tests/fxmods/ (arp, chord, freeverb) are Schwung's own module sources at the same
commit (MIT), built only for the tests.
