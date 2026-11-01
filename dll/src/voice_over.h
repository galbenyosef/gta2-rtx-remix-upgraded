// Voice-overs for the game's text messages.
//
// Every message the game shows is looked up by its text key (the numbers in the
// .gxt files: "1131" is the Zaibatsu Bank Van briefing). This listens for those
// lookups and, when a key has a recording, plays it. Recordings live in
// gta2dx9_voice\ beside the game, one WAV per key, named for the key: 1131.wav.
// Adding a line is just dropping a file in; nothing here changes.
#pragma once

namespace gta2dx9 {

// Reads gta2dx9_voice\*.wav and routes the game's text lookup through us. Leaves
// a gta2.exe that is not the build these addresses came from alone.
void VoiceInstall();

// How loud the car engines are, as a percentage of the game's own level (the menu's slider; the
// default is 70). The game's sound calls are routed through us; an engine is told by its pitch
// being rewritten as the revs change. Available only on the gta2.exe build the addresses came from.
void EngineSoundInstall();
float& EngineVolumePercent();
bool EngineSoundAvailable();
int EngineSoundSlotsSeen();   // how many engines have been told apart so far, for the menu

// The game plays a radio station in stereo near a radio source and swaps to a mono copy further
// out, as if losing the signal. This is the menu's switch for that: off (the default) keeps the
// stereo file at any distance. Takes effect the next time the game picks a source.
void RadioInstall();
bool& RadioReceptionEffect();
bool RadioReceptionAvailable();

}  // namespace gta2dx9
