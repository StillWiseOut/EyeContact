// Shared by the plugin and the test tool.
#pragma once

// "Keep original color": take only the brightness change from Maxine's result and keep the
// source chroma. Maxine tends to repaint irises with saturated, "standard" colors - very visible
// on flat / log footage. Works in any channel range (0..1, 0..255, 0..32768); values are B,G,R.
inline void KeepSourceColor(float sb, float sg, float sr, float& b, float& g, float& r) {
    const float ySrc = 0.0722f * sb + 0.7152f * sg + 0.2126f * sr;  // Rec.709 luma
    const float yOut = 0.0722f * b + 0.7152f * g + 0.2126f * r;
    const float dy = yOut - ySrc;
    b = sb + dy;
    g = sg + dy;
    r = sr + dy;
}
