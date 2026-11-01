// Finding where GTA2 keeps the money and the finished jobs, so they can be saved. See
// progress_probe.cpp.
#pragma once

namespace gta2dx9 {

// Polled once a frame. F9 takes a snapshot of the game's state and logs what changed since the
// last; F10 reads a number from gta2dx9_probe.txt and hunts for it in memory.
void ProgressProbeTick();

}  // namespace gta2dx9
