#pragma once

#include "app/game_source.h"

namespace launcher {

// Appends one line to logs/launcher.log beside the game. Best effort: never throws.
void LogLine(const char* format, ...);

// Returns false when the player closes the launcher without starting the game.
bool Run(app::GameSource& source);
// Starts the game in its own process, so the launcher's shutdown cannot stop it.
// `instance` is passed on for multi-instance runs.
bool LaunchGame(const app::GameSource& source, int instance);
bool LaunchPal(const app::GameSource& source);
bool PalInstalled(app::GameRegion region);

} // namespace launcher
