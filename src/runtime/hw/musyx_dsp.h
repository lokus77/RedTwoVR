#pragma once
// MusyX DSP ucode (MusyX <= 2.0.0) emulated at the command-list level; see musyx_dsp.cpp.
#include <string>

#include "../ctx.h"

namespace rt {

// A CPU->DSP mail while the ucode runs. Returns true with the DSP's answer in `reply` when a
// command list was processed (the caller posts it with a DSP interrupt).
bool musyxDspMail(u32 mail, u32& reply);
std::string musyxDspStats();  // lists and voice frames since the last call
// The discrete channels of an output block the DSP wrote (5.1 / 7.1 output), found by the AI
// DMA's stereo data (nullptr: not one of the last few blocks, or not 160 frames): per frame
// FL, FR, rear L, rear R (5.1's surround pair: the whole rear), back L, back R, side L, side R
// (7.1). Center and LFE are not in the game's mix (MusyX pans between front L and R).
const s16* musyxDiscreteBlock(const u8* stereo, u32 frames);

u8* aramBase();  // hw.cpp: the 16 MiB of ARAM
void aiTraceEvent(char kind, u32 v);  // hw.cpp: R2_AI_LOG's audio event trail

namespace audio {
void init();  // host audio output (SDL3); R2_NO_AUDIO=1 for none, R2_AUDIO_DUMP=<file.wav>
// One AI DMA block as it starts playing: big-endian 16-bit stereo frames.
void push(const u8* data, u32 frames, u32 rate);
// Output gain (0..1, applied by the SDL stream): the menu's volume, muted while the window is
// in the background if the player wants that.
void setGain(float gain);
// The speakers setting (settings::Speakers): the output reopens with 2, 6 or 8 channels when that
// changes what it should be.
void setSpeakers(int speakers);
// The audio_buffer setting (0 normal, 1 large, 2 huge): the cushion playback keeps
void setBuffer(int level);
int outputChannels();  // what the output plays (or would, with no device): 2, 6 or 8
void deviceEvent(const void* sdlEvent);  // every SDL event: playback devices come and go
std::string perfText();                 // what was played since the last call (perf lines)
}  // namespace audio

}  // namespace rt
