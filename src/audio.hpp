#pragma once
// Sounds of the interface and the tools: short clips synthesised at start-up (no sound files), mixed on an audio
// thread. Windows: waveOut; Android: AAudio. Every call is safe without a sound device (it just stays silent).

enum class Sfx {
    Click,     // a button or a tab
    Slide,     // a drawer or a sheet sliding out / back
    Insert,    // synthesiser: a cell placed ("plop")
    Boost,     // booster: a rising zap
    Remove,    // removal: a pop
    Shock,     // electrode: a crackling buzz
    Needle,    // a needle stuck into a cell
    Wall,      // map editor: a wall drawn
    Water,     // map editor: a current drawn
    Count
};

namespace audio {
bool init();      // opens the device and synthesises the clips (false: no sound, nothing else changes)
void shutdown();
void play(Sfx s, float gain = 1.0f);
void setVolume(float v);  // 0..1, 0 = muted
}  // namespace audio
