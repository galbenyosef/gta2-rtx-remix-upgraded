# GTA2 RTX Remix — HD Cars Edition

GTA2 path-traced with [NVIDIA RTX Remix](https://github.com/NVIDIAGameWorks/rtx-remix), with hand-made HD cars,
real glass, doors and dents, working lights and new voice packs.

> **Video:** _gameplay video goes here_

This project is an upgrade of **[gebdag/gta2-rtx-remix](https://github.com/gebdag/gta2-rtx-remix)**, the
Direct3D 9 renderer that makes GTA2 run on RTX Remix. All the renderer work is theirs; thank you.
Everything on top of it (HD cars, car damage, lights, blood, voices and more) is added here.
Both projects are MIT licensed.

## Install (3 steps)

1. **Install RTX Remix** for x86 games into your GTA2 folder. [Remix Plus 1.5.1](https://github.com/RemixProjGroup/dxvk-remix/releases/tag/remix-plus-1.5.1) is recommended.
2. **Download** the zip from [Releases](../../releases) and **unzip it into your GTA2 folder** (the one with `gta2.exe`). Say yes to replacing files.
3. **Start `gta2.exe`.**

That's it. Press **F4** in game for the settings panel (the game's own **Options** button opens it too).

Before step 2, copy `d3ddll.dll`, `Dmavideo.dll` and `DMAGlide.dll` somewhere safe so you can undo the install.
Your player profiles and saves are never touched.

**You need:** your own copy of GTA2 (v9.6 is what it was tested on; this project ships no game files), an
RTX-capable GPU with a current NVIDIA driver, and Windows 10 or 11, 64-bit.

**Where to get GTA2:** see [Where to get GTA2](#where-to-get-gta2) below.

**Something wrong?** Check `gta2dx9.log` next to `gta2.exe`, and the troubleshooting list in `gta2dx9_README.txt`
inside the zip.

## What you get

- Hand-made HD cars with full PBR maps and real glass
- Doors and dents drawn on the HD cars, wheels, roof domes and gang roof logos
- Taxi, police and emergency lights; blood puddles on the road
- Radio stays in stereo at any distance (the original distance effect is a menu option)
- Voice packs
- Everything from the original project: lights from the game's own events, dynamic time of day
  (Remix Plus only), emissive maps, animated water, full 16:9, and 60 fps with frame generation

## Where to get GTA2

GTA2 was officially free for a while: Rockstar gave it away as part of its **Rockstar Classics** series, together with the first
GTA and Wild Metal Country. That free offer has ended. Rockstar later sold it on its own store (until 2013, according to
[Delisted Games](https://delistedgames.com/?p=10001)), and Steam removed its GTA2 page in July 2025 without explanation.

So today there is no official place to get it. The game is still easy to find online if you look for it (searching for
"GTA 2 Rockstar Classics" is a good start), but those copies come from third parties, so use one you trust, or a copy you
already own (the original CD, or an old Rockstar or Steam purchase). This project ships no game files and does not host or
link to downloads of the game.

## Known issues

The tank turret still uses the original low-resolution art, the 5-star FBI cars are blue instead of black,
and a few cars still have odd windows. More in the [release notes](../../releases).

## Uninstall

Put your backed-up `d3ddll.dll`, `Dmavideo.dll` and `DMAGlide.dll` back, run `uninstall.reg`, and delete the
`gta2dx9*` files, `gta2dx9_hd`, `gta2dx9_voice` and `rtx-remix\mods\gta2rtx` and `gta2water`.

## Building from source

Needs Visual Studio with the C++ x86 toolset.

```bat
package.bat
```

Builds `dll\build\gta2dx9.dll` and `vid\build\gta2dx9_vid.dll` and assembles the release files in `release\`.
The two DLLs can also be built alone with `dll\build.bat` and `vid\build.bat`. The RTX Remix API headers in
`dll\deps\bridge_api` must match the Remix runtime the game uses (see `dll\deps\README.md`).

```
dll/        gta2dx9.dll, the renderer
vid/        gta2dx9_vid.dll, the video device shim
src/        map, style, mesh, camera and D3D9 renderer code used by the DLL
package/    release files: settings, registry files, player README, tuned ini files
tools/      development tools
docs/       research notes
```

Technical notes on the renderer are in `dll/README.md`.

## Legal

MIT licence, see [LICENSE](LICENSE); third-party components are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
This is an unofficial fan project. GTA2 is a trademark and copyright of Rockstar Games / Take-Two Interactive,
and this repository and its releases contain no game files.
