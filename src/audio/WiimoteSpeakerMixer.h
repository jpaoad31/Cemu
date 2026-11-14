#pragma once

// Wii Remote speaker sound played through Cemu's own audio output ("system audio").
//
// The speaker HLE (padscore) decodes what a game streams to a remote's speaker and pushes the
// 16-bit samples here, per WPAD channel, at the speaker's rate (6 kHz for the usual ADPCM stream).
// AX's output stage asks for each 3 ms frame at 48 kHz and adds it to the TV mix only, like
// Dolphin's emulated speaker does with its mixer. Never the GamePad: on a real Wii U the GamePad
// has its own audio stream, and Wii Remote sound doesn't come out of it.
//
// Each channel has a small jitter buffer: playback starts once kStartBufferMs is queued (the game
// sends in bursts of emulated time, the output pulls in real time) and an empty buffer goes quiet
// and waits for kStartBufferMs again. To keep the delay steady without audible jumps, playback runs
// slightly faster or slower (at most kMaxRateShift) while the averaged buffer level is above or
// below kStartBufferMs: the sample-rate correction of Dolphin's MixerFifo (AudioCommon/Mixer.cpp,
// GPL-2.0-or-later; the idea and its constants, not its code). More than kMaxBufferMs queued is
// still trimmed, as a backstop (e.g. after the emulator was paused).

#include <cstddef>
#include <cstdint>

namespace WiimoteSpeakerMixer
{
	constexpr size_t kChannels = 4; // WPAD channels with a speaker
	constexpr uint32_t kOutputRate = 48000;
	constexpr uint32_t kStartBufferMs = 30;
	constexpr uint32_t kMaxBufferMs = 150;
	// rate correction, as in Dolphin's mixer: shift = (average level - target) * kRateControl Hz,
	// the level averaged over kRateAverage output frames, the shift capped at kMaxRateShift of the rate
	constexpr double kRateControl = 0.2;
	constexpr double kRateAverage = 32.0;
	constexpr double kMaxRateShift = 0.00625;

	// an extra gain (1.0 = as decoded)
	void SetGain(float gain);

	// samples of one channel at `sampleRate`; `volume` (0..1, the speaker's volume register) is applied on output
	void Push(size_t channel, const int16_t* samples, size_t count, uint32_t sampleRate, float volume);
	// the stream of a channel stopped (speaker off / muted / disconnected): drop what's queued
	void Clear(size_t channel);
	void ClearAll();

	// Renders `frames` samples at kOutputRate into `out` (mono, for the TV mix; `out` is
	// overwritten). Returns false (and leaves `out` alone) if no channel is playing.
	bool Render(float* out, size_t frames);

	struct Stats
	{
		uint64_t pushed = 0;     // input samples
		uint64_t played = 0;     // input samples consumed
		uint64_t underruns = 0;  // buffer ran empty while playing
		uint64_t trimmed = 0;    // input samples dropped because too much was queued
		size_t queued = 0;       // input samples now
		bool playing = false;
	};
	Stats GetStats(size_t channel);
}
