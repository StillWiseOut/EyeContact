// EyeContact - eye contact correction for Premiere Pro / After Effects.
// Thin wrapper around the NVIDIA Maxine AR SDK "GazeRedirection" feature.
// This header deliberately does not include any NVIDIA headers.
#pragma once

#include <cstdint>
#include <string>

// Settings that require the Maxine feature to be (re)loaded when they change.
struct EngineConfig {
    bool  temporal           = true;   // temporal filtering of landmarks (smoother, needs sequential frames)
    int   eyeSizeSensitivity = 3;      // 2..5
    bool  lookAway           = false;  // random natural look-aways
    float gazePitchLow  = 20.f, gazePitchHigh = 30.f;
    float gazeYawLow    = 20.f, gazeYawHigh   = 30.f;
    float headPitchLow  = 15.f, headPitchHigh = 25.f;
    float headYawLow    = 25.f, headYawHigh   = 30.f;

    bool operator==(const EngineConfig&) const = default;
};

// Appends a line to %TEMP%\EyeContact.log (printf-style).
void EyeContactLog(const char* fmt, ...);

class MaxineEngine {
public:
    // Process-wide singleton. Intentionally leaked so nothing touches CUDA during DLL unload.
    static MaxineEngine& Instance();

    // Runs gaze redirection in place on a tightly packed 8-bit BGR image (width*height*3 bytes).
    // Thread-safe (serialized internally). Returns false on failure; *error then holds a message.
    // An empty *error with false means "nothing to do" (should not normally happen).
    bool Process(uint8_t* bgr, int width, int height, const EngineConfig& cfg, std::string* error);

    // Frees all GPU resources. Called from PF_Cmd_GLOBAL_SETDOWN.
    void Shutdown();

private:
    MaxineEngine();
    struct Impl;
    Impl* p_;
};
