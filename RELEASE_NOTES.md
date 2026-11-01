# GTA2 RTX Remix mod — release notes (draft)

A Direct3D 9 renderer for GTA2 that lets RTX Remix path-trace the game, built on
[gebdag/gta2-rtx-remix](https://github.com/gebdag/gta2-rtx-remix) (MIT). This release adds a hand-made visual layer on top.

## Install

1. Install RTX Remix for x86 games (Remix Plus 1.5.1 recommended) into your GTA2 folder.
2. Back up `d3ddll.dll`, `Dmavideo.dll` and `DMAGlide.dll`, then unzip this release into the GTA2 folder
   (the one with `gta2.exe`), replacing them.
3. Start the game. **F4** (or the game's own **Options** button) opens the settings panel.

The zip contains no game files, no RTX Remix binaries and no player profiles, and it overwrites none.

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

## Publishing with the GitHub CLI

```bat
cd /d G:\gta2-rtx-remix-carbodies-src
git add -A
git commit -m "Release"
git push
gh release create v2026.11.01 "G:\GTA2_RTX_Remix_mod_2026.11.01.zip" --title "GTA2 RTX Remix mod 2026.11.01" --notes-file RELEASE_NOTES.md
```
