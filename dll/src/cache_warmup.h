// Reads the mod's own files into the operating system's file cache ahead of need.
//
// After a restart nothing the mod uses is in memory yet: the first time each car turns up
// its hand-made maps and the recoloured artwork made for its paint are read from disk, and
// on a cold disk that is a stall on the frame. This runs on a thread of its own, at the
// lowest priority, and reads the files that were used last time (and the hand-made
// artwork) so they are already in memory when the game asks. It keeps a short list of the
// files each session used, gta2dx9_warm_list.txt, so the warm-up follows what you play.
//
// Settings, in the [cache] section of gta2dx9_effects.ini:
//   warmup=1            0 turns it off
//   max_megabytes=2048  the most it reads
//   prune_days=10       cache files not used for this many days are deleted (0 keeps everything)
//
// The cache folder otherwise only grows: a changed setting makes new files and the old
// ones are never touched again. After one session has built the list of files in use,
// the next start deletes the ones neither used nor written for prune_days.
#pragma once

#include <string>

namespace gta2dx9 {

// Starts the thread. Call once, after the game's window and the ini files are known.
void CacheWarmupStart();

// A file the mod just used from the cache or the artwork folder, so the next session can
// read it ahead. Cheap, safe from any thread, and meant for the moment a material is made,
// not for every frame.
void CacheWarmupNote(const std::wstring& path);

}  // namespace gta2dx9
