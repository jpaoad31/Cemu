#pragma once

// Wii Remote speaker: sample formats, register values and stream timing.
// Standard headers only, so it can be tested on its own (WiimoteSpeakerCodecTest.cpp).
//
// The speaker plays 4-bit Yamaha ADPCM or 8-bit signed PCM. Output report 0x18 carries up to 20
// bytes: 40 ADPCM samples (high nibble first) or 20 PCM samples. The configuration is 7 bytes
// written to register 0xA20001: [0x00][format][rate, u16 little-endian][volume][0x0C][0x0E].
//
// The ADPCM decoder is adapted from Dolphin (Source/Core/Core/HW/WiimoteEmu/Speaker.cpp,
// GPL-2.0-or-later, itself based on FFmpeg's Yamaha ADPCM decoder). The encoder is written to
// match that decoder: it picks the nibble whose reconstruction is closest to the input and then
// updates its state with the decoder's own step, so encoder and decoder never drift apart.

#include <cstddef>
#include <cstdint>
#include <chrono>

namespace WiimoteSpeakerCodec
{
	constexpr uint8_t kFormatAdpcm = 0x00; // 4-bit Yamaha ADPCM
	constexpr uint8_t kFormatPcm8 = 0x40;  // 8-bit signed PCM

	constexpr size_t kReportDataSize = 20; // payload bytes of one 0x18 report

	// The configuration WPADControlSpeaker(ON) writes on a Wii / Wii U: ADPCM, rate value
	// 0x07D0, the volume from the system settings
	constexpr uint16_t kDefaultRateValue = 0x07D0;
	constexpr uint8_t kMaxVolumeAdpcm = 0x7F;
	constexpr uint8_t kMaxVolumePcm8 = 0xFF;
	constexpr uint8_t kDefaultVolume = 89; // the Wii's speaker volume when none is set (0..127)

	struct Config
	{
		uint8_t format = kFormatAdpcm;
		uint16_t rate = kDefaultRateValue;
		uint8_t volume = kDefaultVolume;

		// samples per second the speaker plays.
		// ADPCM: 12 MHz / rate (6000 Hz for 0x07D0: the 40 samples per report the Wii SDK sends every
		// 6.67 ms). PCM8: 24 MHz / rate, following Dolphin (which plays both formats at twice the
		// wiibrew rate); no Wii U title is known to use PCM.
		uint32_t SampleRate() const;
		// samples in one report of `bytes` payload bytes
		uint32_t SamplesPerReport(size_t bytes = kReportDataSize) const;
		// time one full report lasts at SampleRate()
		std::chrono::nanoseconds ReportInterval(size_t bytes = kReportDataSize) const;
		// 0.0 .. 1.0 (volume relative to the format's maximum)
		float Gain() const;
	};

	// the 7 bytes for register 0xA20001
	void BuildConfigRegister(const Config& config, uint8_t out[7]);

	struct AdpcmState
	{
		int32_t predictor = 0;
		int32_t step = 127;
	};

	int16_t DecodeAdpcmNibble(AdpcmState& state, uint8_t nibble);
	uint8_t EncodeAdpcmSample(AdpcmState& state, int16_t sample);

	// `samples` PCM samples -> (samples + 1) / 2 bytes, first sample in the high nibble
	size_t EncodeAdpcm(AdpcmState& state, const int16_t* pcm, size_t samples, uint8_t* out);
	// `bytes` bytes -> 2 * bytes samples
	size_t DecodeAdpcm(AdpcmState& state, const uint8_t* data, size_t bytes, int16_t* out);
	// `bytes` bytes -> `bytes` samples
	size_t DecodePcm8(const uint8_t* data, size_t bytes, int16_t* out);

	// Sends reports at a fixed rate: report n is due at start + n * interval. If the sender falls
	// more than kMaxLagReports behind (a stall), the schedule restarts at the current time instead
	// of bursting the missed reports, since the remote's buffer is tiny.
	class StreamPacer
	{
	public:
		using clock = std::chrono::steady_clock;
		static constexpr int kMaxLagReports = 4;

		void Reset() { m_started = false; m_resyncs = 0; m_sent = 0; }
		void SetInterval(std::chrono::nanoseconds interval) { m_interval = interval; }
		std::chrono::nanoseconds Interval() const { return m_interval; }

		// true if the next report may go now (starts the schedule on the first call)
		bool IsDue(clock::time_point now);
		// marks the report as sent and schedules the next one
		void OnSent(clock::time_point now);
		// when the next report is due (only meaningful once started)
		clock::time_point NextDue() const { return m_next_due; }

		uint64_t Sent() const { return m_sent; }
		uint32_t Resyncs() const { return m_resyncs; }

	private:
		bool m_started = false;
		std::chrono::nanoseconds m_interval{6666667};
		clock::time_point m_next_due{};
		uint64_t m_sent = 0;
		uint32_t m_resyncs = 0;
	};
}
