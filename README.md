# 3dJelly

3dJelly is a 3rd party Nintendo 3DS homebrew client for Jellyfin. It is built for browsing and playing media from a Jellyfin server.

The project is early, there will be bugs. 3dJelly is being actively worked on.

## Features

- Username and password login through Jellyfin
- Saved server, user, and token configuration on the SD card
- Library and item browsing
- 144p, 240p, 360p, and 480p quality modes
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
A      Open or play selected item
B      Back or stop playback
X      Refresh current view
Y      Setup, or audio mute toggle during MJPEG playback
L/R    Change quality mode
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

- Korean/Hangul text is romanized because the default 3DS font does not render Hangul glyphs.
- Playback may crash, particularly at higher resolutions. Lower quality modes are recommended for stability.
- playback may hitch when audio is loud

### Roadmap

Planned amd possible future features or changes

- being able to skip through videos
- a continue watching and up next
- translations. 3dJelly only currently supports English
- Possibly making 3dJelly work on emulators
- a massive rework on UI including seeing metadata, trailers, libraries displayed better
- downloading feature to watch offline
- subtitle support
- add intro skipper and other plugin support

## Credits

3dJelly is developed by 8 Bit Studio.

[Jellyfin](https://github.com/jellyfin/jellyfin) is a separate open source media server project. 3dJelly is an unofficial client and not affiliated with jellyfin.
