#pragma once

namespace hs {

bool agentRunning();
bool launchAgentIfNeeded();

// Stops every running copy of the background service. An install replaces the
// service bundle, but the copy that is already running keeps the old shared
// memory layout and keeps answering instead of the new one, so a plug-in that
// talks to it gets no heartbeat at all. Asking the old copies to quit lets the
// next launchAgentIfNeeded() start the freshly installed one.
bool restartAgent();

} // namespace hs
