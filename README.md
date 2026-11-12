# uHound - A Wii U Hound client

A native [Hound Media Server](https://github.com/Hound-Media-Server/hound) client for the Nintendo Wii U, forked from [Ufin](https://github.com/antu2010/Ufin-A-Wii-U-Jellyfin-client) (a Jellyfin Wii U client) and rebuilt around Hound's API.

**Beta.** Expect bugs. Movies and TV shows from your Hound server can be browsed, searched and played on real hardware.

## What it does

* Sign in to your Hound server (username + password, default port **2323**)
* Browse trending movies and trending shows
* Browse shows: seasons -> episodes, with watched ticks and resume bars
* Browse your collections and your watchlist (favourites)
* **Continue watching** on Home, with resume positions
* Search movies and shows with the Wii U on-screen keyboard
* Play movies and episodes through Hound's stream proxy, with hardware H.264 decoding
* Seek, pause/resume, resume where you stopped
* Mark watched/unwatched, add to/remove from the watchlist
* Playback progress reported back to Hound (continue watching + auto-watched)
* Posters and backdrops straight from TMDB, loaded in the background
* GX2 + Dear ImGui interface, GamePad / Pro Controller / Classic Controller / Wii Remote support
* Menu music, Liquid Glass UI, profiles ("Who's watching?"), and everything else inherited from Ufin

### Not supported (yet)

* **No transcoding:** Hound streams files as they are. The Wii U can only hardware-decode **H.264 in MP4**, so media in other formats (HEVC, AVI, MKV containers...) will fail in the decoder until Hound's server-side transcoding lands.
* **No audio/subtitle track selection:** streams play their default tracks.
* **No music libraries / Live TV:** Hound doesn't have them.

## Installation

1. Download the ZIP from the "Releases" page and extract it to the root of your Wii U's SD card (or build from source, below).
2. Optional: pre-configure the app by editing `config.json`:

   ```json
   {
     "host": "192.168.1.43",
     "port": 2323,
     "username": "your-user",
     "password": "your-password"
   }
   ```

3. Launch `uhound.wuhb` from the Wii U menu (Aroma) or the Homebrew Launcher.

## Signing in

Start uHound and enter your Hound server's address (e.g. `192.168.1.100` or `192.168.1.100:2323`) plus your Hound username and password. The sign-in token (never the password) is saved to `sd:/wiiu/apps/uhound/config.json`, and every sign-in becomes a profile on the "Who's watching?" screen. A hand-written `config.json` with `host`/`port`/`username`/`password` works too.

## SD card layout

```text
SD:/
└── wiiu/
    └── apps/
        └── uhound/
            ├── uhound.wuhb
            ├── config.json
            └── assets/
                ├── bgm.m4a        (optional menu music)
                └── models/        (optional Help-window 3D models)
```

## How playback works

Hound has no server-side transcoding yet, so the client direct-plays what the server has: media files downloaded by Hound first, then Stremio provider streams. Every stream is proxied by Hound through `/api/v1/stream/<encoded data>` and served with byte ranges, which is what makes seeking work: the player re-issues a ranged GET and restarts the demuxer at the new position.

The Wii U's hardware decoder (`h264_wiiu` from [FFmpeg-wiiu](https://github.com/GaryOderNichts/FFmpeg-wiiu)) only handles H.264-in-MP4; frames are decoded on hardware, uploaded as two GX2 textures (NV12) and converted to RGB by a small pixel shader. Video is paced against the audio clock.

## Building

1. Install devkitPro with `wiiu-dev` and `wiiu-sdl2`.
2. Build [FFmpeg-wiiu](https://github.com/GaryOderNichts/FFmpeg-wiiu) **with `patches/ffmpeg-wiiu-fixes.patch` applied** and install it (`./configure-wiiu && make -j$(nproc) && sudo -E make install`).
3. Put `glslcompiler.elf` from [CafeGLSL](https://github.com/Exzap/CafeGLSL/releases) in `tools/`, then:

   ```sh
   mkdir build && cd build && cmake .. && make
   ```

`tools/check_wiiu_build.sh` checks the objects for thread-local storage, which the RPX format can't hold.

### Host tests

Everything that doesn't touch wut/GX2 is covered by host tests (client, config, UI rendering with Dear ImGui, player logic, and -- with a host FFmpeg build -- the real decoder chain):

```sh
tests/host/run_tests.sh
# with a static FFmpeg install prefix:
FFMPEG_HOST=/path/to/ffmpeg-prefix tests/host/run_tests.sh
```

## Credits

* [Ufin](https://github.com/antu2010/Ufin-A-Wii-U-Jellyfin-client) by antu2010 (with major v0.2 work by LandValueGen) -- the Jellyfin Wii U client this fork started from.
* [Hound Media Server](https://github.com/Hound-Media-Server/hound) -- the server this client talks to.
* [FFmpeg-wiiu](https://github.com/GaryOderNichts/FFmpeg-wiiu) by GaryOderNichts -- the Wii U FFmpeg backend.
* UI layout inspired by [CaféMP](https://github.com/whateveritwas/cafemp).

Libraries in `src/vendor/` (licence texts alongside): [Dear ImGui](https://github.com/ocornut/imgui) (MIT) with the Wii U GX2 renderer from [dkosmari/imgui](https://github.com/dkosmari/imgui), [stb_image](https://github.com/nothings/stb) (public domain / MIT), [cJSON](https://github.com/DaveGamble/cJSON) (MIT), [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) (Apache-2.0, trimmed to a TLS 1.2 client).

Model credits in `model credits.md`.

## Contributing

Bug reports and testing feedback are welcome -- especially from real hardware. When reporting, include: real Wii U or Cemu, Hound server version, the media's format/resolution, your network setup, steps to reproduce, and any `uHound:` log output (`Settings > Logging` writes `uhound_log.txt` to the SD card).
