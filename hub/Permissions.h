#pragma once

#include <functional>

// System-audio-recording and microphone permission helpers. The Agent is the
// only process that owns these grants; the plug-ins never ask.
void requestAudioCapturePermission(std::function<void(bool)> callback);
void requestMicrophonePermission(std::function<void(bool)> callback);

// -1 = unknown, 0 = denied, 1 = undecided, 2 = granted.
int audioCapturePermissionRaw();
int microphonePermissionRaw();
