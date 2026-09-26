# 3dJelly

3dJelly is a 3rd party Nintendo 3DS homebrew client for Jellyfin. It is built for browsing and playing media from a Jellyfin server.

The project is early, there will be bugs. 3dJelly is being actively worked on.

## Features

- Username and password login through Jellyfin
- Quick Connect sign-in using approval from another signed-in Jellyfin device
- Automatic local Jellyfin server discovery with tap-to-select results (UDP port 7359)
- Touchscreen User / Pass form with the server's branding splashscreen
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

On first launch, select a Jellyfin server found on your Wi-Fi, or tap **Add Server**
to enter its address. Tap **Search Again** (or press X) to repeat discovery.
The server must allow UDP port 7359 and be reachable from the 3DS's network.
Discovery retries subnet broadcasts and probes UDP port 7359 directly on the
local subnet (up to 1024 addresses; the device's /24 on larger networks).

After selecting a server, tap **User**, **Pass**, then **Sign In**. D-pad and A
also work. **Change Server** returns to discovery. Saved sessions still sign in
automatically without flashing the sign-in form. While a saved session connects,
a neutral loading screen is shown; B opens sign-in explicitly. Connection or
session failures return to setup. Clear the saved login in Settings to sign out.

For password-free sign-in, tap **Use Quick Connect** below **Sign In**. On a phone
or computer already signed in to the same Jellyfin server, open **Settings >
Quick Connect**, enter the six-character code shown on the 3DS, and approve it.
The 3DS signs in automatically and saves the session. The code expires after
10 minutes; **Try Again** (or X) requests a new code after an error or expiry.
**Cancel** (or B) returns to the sign-in form. Quick Connect must be enabled on
the server; username/password sign-in remains available when it is disabled.
See [Jellyfin's Quick Connect guide](https://jellyfin.org/docs/general/server/quick-connect/).

The background uses Jellyfin's enabled branding splashscreen
(`/Branding/Splashscreen?format=jpg`), with a dark fallback when unavailable.
It loads in the background and caches the decoded image on the SD card. Large
wallpapers use full-detail decoding and averaged downsampling because the splashscreen endpoint returns
the original dimensions. Download/decode progress is shown on the sign-in screen;
diagnostics are saved to `sdmc:/3dJelly/setup-network.log`. An
unreachable saved server returns to discovery without blocking the initial UI.

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

On Windows with devkitPro MSYS2 and Python installed, run
`python tests/run_quick_connect_tests.py` for the Quick Connect HTTP regression
tests. They exercise approval, retries, disabled servers, expiry, cancellation,
malformed responses, and saved-session protection against a local API fixture.

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
