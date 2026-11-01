# GTA2 RTX Upgraded 1.1

A Direct3D 9 renderer for GTA2 that lets RTX Remix path-trace the game, built on
[gebdag/gta2-rtx-remix](https://github.com/gebdag/gta2-rtx-remix) (MIT). This release adds a hand-made visual layer on top.

## Install

1. **Install RTX Remix:** download `Remix_Plus_v1.5.1_x86_games_release.zip` from the **Assets** list at the bottom of the
   [Remix Plus 1.5.1 release page](https://github.com/RemixProjGroup/dxvk-remix/releases/tag/remix-plus-1.5.1) and unzip it into your
   GTA2 folder next to `gta2.exe`. (Without it the game runs, but without ray tracing.)
2. **Unzip this release** into the same GTA2 folder. Say yes to replacing files.
3. **Start the game.** **F4** (or the game's own **Options** button) opens the settings panel.

The zip contains no game files, no RTX Remix binaries and no player profiles. Saves are never touched.

## Where to get GTA2

GTA2 was officially free for a while: Rockstar gave it away as part of its **Rockstar Classics** series, together with the first
GTA and Wild Metal Country. That free offer has ended. Rockstar later sold it on its own store (until 2013, according to
[Delisted Games](https://delistedgames.com/?p=10001)), and Steam removed its GTA2 page in July 2025 without explanation.

So today there is no official place to get it. The game is still easy to find online if you look for it (searching for
"GTA 2 Rockstar Classics" is a good start), but those copies come from third parties, so use one you trust, or a copy you
already own (the original CD, or an old Rockstar or Steam purchase). This project ships no game files and does not host or
link to downloads of the game.

## What's new in 1.1

- **Fixed:** the car radio no longer goes silent when you drive away from its source. It now stays in stereo and fades out
  with distance, as intended.

## What's in it

- Hand-made HD cars with full PBR maps (albedo, normal, roughness, metal) and real glass
- Doors and dents drawn as shading on the HD cars; wheels, roof domes, gang roof logos
- Taxi, police and emergency lights; blood puddles on the road
- Radio stays in stereo at any distance (original distance effect is a menu option)
- Voice packs for gangs, cops and pedestrians
- Emissive maps and animated water as Remix mods

## Known issues

- Tank turret still uses the game's original low-resolution art and looks out of place next to the HD hull.
- 5-star FBI cars show blue instead of black.
- Some cars still have unmarked windows or false glass; a few sprites (pink sports car, limo) are still low-res.
- Daytime light trails (denoiser), pedestrian flicker and dusk reflections need more work.

## Credits and licence

MIT. See `gta2dx9_LICENSE.txt` and `gta2dx9_THIRD_PARTY.txt` (Dear ImGui, NVIDIA RTX Remix API headers).
GTA2 is a trademark and copyright of Rockstar Games / Take-Two. Unofficial fan project.
