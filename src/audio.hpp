#pragma once
// Sounds, in the manner of the original Cell Lab: its only sound is the system key click when a button is tapped.
// On Android the game plays that very click; elsewhere a synthesised soft tick stands in for it. The tools have
// optional soft sounds of their own (off by default: the original is silent there). Short clips are synthesised at
// start-up (no sound files) and mixed on an audio thread. Windows: waveOut; Android: AAudio. Every call is safe
// without a sound device (it just stays silent).

enum class Sfx {
    Click,     // a button or a tab
    Insert,    // synthesiser: a cell placed (a bubble)
    Boost,     // booster: a soft rising swell
    Remove,    // removal: a low pop
    Shock,     // electrode: a short crackle
    Needle,    // a needle stuck into a cell
    Wall,      // map editor: a wall drawn (a light knock)
    Water,     // map editor: a current drawn
    Count
};

namespace audio {
bool init();      // opens the device and synthesises the clips (false: no sound, nothing else changes)
void shutdown();
void play(Sfx s, float gain = 1.0f);
void setVolume(float v);       // 0..1, 0 = muted
void setToolSounds(bool on);   // the tools' own sounds (Insert..Water); the click obeys only the volume
}  // namespace audio
