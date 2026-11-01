// Saving the game in the middle of a level, for free.
//
// The church in GTA2 saves the game for $50,000. That is a money check in front of a plain
// function in gta2.exe (0x47EF40, thiscall on the script manager, taking the save's name) that
// writes player\plyslotN.svg. This calls the same function without the charge - from a button in
// F4, and automatically a moment after a big payout, which is what finishing a job looks like.
#pragma once

namespace gta2dx9 {

struct GameSaveSettings {
    bool autoSave = true;      // save by itself after a big payout
    int minPayout = 3000;      // the smallest rise in money that counts as a finished job
    float delay = 2.0f;        // seconds to wait after the payout, for the game to finish its own bookkeeping
    bool message = true;       // show the game's own "game saved" message
};

GameSaveSettings& GameSave();

// Writes the save now. False if the game was not in a state to be saved.
bool SaveGameNow();

// Seconds since the last save made here (negative if none yet), and the last result, for the menu.
double GameSaveAgeSeconds();
const char* GameSaveStatus();
int GameSaveMoney();   // the money the game reports now, or -1

// Polled once a frame: watches the money and saves after a payout.
void GameSaveTick();

void GameSaveLoadSettings();
bool GameSaveSaveSettings();

}  // namespace gta2dx9
