#pragma once

#include <stdbool.h>

// Permission guide window. Shows which grant is missing, lets the user drag the
// app straight into the matching System Settings list, and watches the grant so
// it closes itself once the switch is flipped.
//
// The agent owns this window: plug-ins and the command line only ask for it.
#ifdef __cplusplus
extern "C" {
#endif

void hsShowPermissionGuide(void);

// Called from the agent's tick. Does nothing unless a permission that the
// current capture mode needs is still missing.
void hsShowPermissionGuideIfNeeded(void);

// QA hook: render the guide into a PNG at `path` and return whether it was
// written. Rendering happens inside the app, so it needs no Screen Recording
// grant and works from a headless test run.
bool hsSnapshotPermissionGuide(const char* path);

#ifdef __cplusplus
}
#endif
