#pragma once

namespace photobridge {

// Deterministic process-level crash-window hook. It is inert unless the named
// environment variable contains a positive millisecond duration.
void PauseForTest(const char* environment_name);

}  // namespace photobridge
