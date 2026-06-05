# 3dJelly

3dJelly is a 3rd party Nintendo 3DS homebrew client for Jellyfin. It is built for browsing and playing media from a Jellyfin server.

The project is early, there will be bugs. 3dJelly is being actively worked on.

## Features

- Username and password login through Jellyfin
- Saved server, user, and token configuration on the SD card
- Library and item browsing
- 144p, 240p, and 240HQ available on old Nintendo 3ds systems. 360p and 480p are available on New Nintendo 3ds systems.
- Experimental New 3DS H.264 hardware decode path

## Downloads

Download the latest CIA from the Releases page:

https://github.com/8-bitStudio/3d-jelly/releases

Install `3dJelly.cia` on a modded Nintendo 3DS, or use it with an emulator that supports CIA installation.

3dJelly works best on the 'New Nintendo 3ds' but works on the original Nintendo 3ds.

## Current Status

3dJelly is a prototype. Browsing and login are the most stable parts right now. Playback is still experimental because the 3DS has limited CPU power, limited memory, and different video capabilities between Old 3DS, New 3DS, and emulators.

Current playback paths:

- New 3DS: tries Jellyfin transcoding to low resolution H.264 and uses the 3DS MVD hardware decoder.
- Old 3DS: defaults to 144p and uses a Jellyfin MJPEG fallback path.
- Azahar: can use the fallback path for testing when hardware video decode is not available.

## Server Setup

Use the local network address of your Jellyfin server. For example:

```text
http://YOUR_SERVER_IP:8096
```

Do not use `localhost` unless Jellyfin is running inside the same device or emulator environment. For a real 3DS, use the IP address of the computer or server running Jellyfin.

## Controls

```text
A              Open/play selected item, pause/resume during playback
B              Back, stop playback, or cancel autoplay countdown
X              Refresh current view
Y              Settings from browse screens, mute/unmute during playback
D-Pad Up/Down  Volume during playback
D-Pad Left/Right Change quality during playback
L/R            Scrub backward/forward during playback
START          Exit
```

## Building

Build from devkitPro MSYS2:

```sh
make
```

Build the CIA:

```sh
make cia
```

Regenerate the Home Menu icon:

```sh
make icon
```

CIA packaging requires `makerom.exe`. Put it at `tools/makerom.exe` or make it available on `PATH`.

## Project Layout

```text
source/             3DS client source code
tools/              CIA packaging and icon generation scripts
gfx/icon.png        Generated 3DS Home Menu icon
dist/3dJelly.cia    Current installable CIA build
Makefile            devkitPro build file
```

## Known Issues

- playback may hitch when audio is loud
- some shows make you click B twice to get out of specials
- some shows dont show season 2

### Roadmap

Planned and possible future features or changes:

- Continue watching and up next
- Better emulator compatibility
- Improved UI with metadata, trailers, and better library views
- Offline downloads
- Subtitle support
- Intro skipper and plugin support

## Credits

Developed by [8-bit Studio](https://github.com/8-bitStudio) and [contributors](https://github.com/8-bitStudio/3d-jelly/graphs/contributors). 

## Built With

- C and Make
- [devkitPro / devkitARM](https://github.com/devkitPro) for Nintendo 3DS homebrew building
- [libctru](https://github.com/devkitPro/libctru) for 3DS system services, input, filesystem, networking, HTTP, audio, and app lifecycle
- [citro2d](https://github.com/devkitPro/citro2d) and [citro3d](https://github.com/devkitPro/citro3d) for native 3DS rendering
- [Mbed TLS / mbedcrypto](https://github.com/Mbed-TLS/mbedtls) for AES and SHA-256 used by saved credential encryption
- [picojpeg](https://github.com/richgel999/picojpeg) for JPEG/MJPEG frame decoding
- [pl_mpeg](https://github.com/phoboslab/pl_mpeg) for the experimental MPEG-1/MP2 playback code path
- [Project_CTR makerom](https://github.com/3DSGuy/Project_CTR) for CIA packaging

[Jellyfin](https://github.com/jellyfin/jellyfin) is a separate open source media server project. 3dJelly is an unofficial client and not affiliated with Jellyfin.
