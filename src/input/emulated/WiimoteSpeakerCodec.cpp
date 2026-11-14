#include "input/emulated/WiimoteSpeakerCodec.h"

#include <algorithm>
#include <cstdlib>

namespace WiimoteSpeakerCodec
{
	uint32_t Config::SampleRate() const
	{
		if (rate == 0)
			return 0;
		if (format == kFormatPcm8)
			return 24000000u / rate;
		return 12000000u / rate;
	}

	uint32_t Config::SamplesPerReport(size_t bytes) const
	{
		return format == kFormatPcm8 ? (uint32_t)bytes : (uint32_t)(bytes * 2);
	}

	std::chrono::nanoseconds Config::ReportInterval(size_t bytes) const
	{
		const uint32_t sample_rate = SampleRate();
		if (sample_rate == 0)
			return std::chrono::nanoseconds(0);
		return std::chrono::nanoseconds((int64_t)SamplesPerReport(bytes) * 1000000000ll / sample_rate);
	}

	float Config::Gain() const
	{
		const float max = format == kFormatPcm8 ? (float)kMaxVolumePcm8 : (float)kMaxVolumeAdpcm;
		return std::min(1.0f, (float)volume / max);
	}

	void BuildConfigRegister(const Config& config, uint8_t out[7])
	{
		out[0] = 0x00;
		out[1] = config.format;
		out[2] = (uint8_t)(config.rate & 0xFF);
		out[3] = (uint8_t)(config.rate >> 8);
		out[4] = config.volume;
		out[5] = 0x0C;
		out[6] = 0x0E;
	}

	// Yamaha ADPCM, as decoded by Dolphin's emulated speaker (from FFmpeg)
	static constexpr int32_t kDiffLookup[16] = {1, 3, 5, 7, 9, 11, 13, 15,
	                                            -1, -3, -5, -7, -9, -11, -13, -15};
	static constexpr int32_t kIndexScale[16] = {230, 230, 230, 230, 307, 409, 512, 614,
	                                            230, 230, 230, 230, 307, 409, 512, 614};

	static int32_t Clip16(int32_t a)
	{
		return std::clamp(a, -32768, 32767);
	}

	static int32_t NextPredictor(const AdpcmState& state, uint8_t nibble)
	{
		return Clip16(state.predictor + (state.step * kDiffLookup[nibble]) / 8);
	}

	static void Advance(AdpcmState& state, uint8_t nibble)
	{
		state.predictor = NextPredictor(state, nibble);
		state.step = std::clamp((state.step * kIndexScale[nibble]) >> 8, 127, 24576);
	}

	int16_t DecodeAdpcmNibble(AdpcmState& state, uint8_t nibble)
	{
		nibble &= 0xF;
		Advance(state, nibble);
		return (int16_t)state.predictor;
	}

	uint8_t EncodeAdpcmSample(AdpcmState& state, int16_t sample)
	{
		// the nibble whose reconstruction is closest to the sample (ties: the smaller step, which
		// keeps the step size down); only the half with the right sign can win, but trying all 16
		// is cheap and also handles clipping at the 16-bit limits
		uint8_t best = 0;
		int32_t best_error = INT32_MAX;
		for (uint8_t nibble = 0; nibble < 16; nibble++)
		{
			const int32_t error = std::abs(NextPredictor(state, nibble) - (int32_t)sample);
			if (error < best_error)
			{
				best_error = error;
				best = nibble;
			}
		}
		Advance(state, best);
		return best;
	}

	size_t EncodeAdpcm(AdpcmState& state, const int16_t* pcm, size_t samples, uint8_t* out)
	{
		const size_t bytes = (samples + 1) / 2;
		for (size_t i = 0; i < bytes; i++)
			out[i] = 0;
		for (size_t i = 0; i < samples; i++)
		{
			const uint8_t nibble = EncodeAdpcmSample(state, pcm[i]);
			out[i / 2] |= (i & 1) ? nibble : (uint8_t)(nibble << 4);
		}
		return bytes;
	}

	size_t DecodeAdpcm(AdpcmState& state, const uint8_t* data, size_t bytes, int16_t* out)
	{
		for (size_t i = 0; i < bytes; i++)
		{
			out[i * 2] = DecodeAdpcmNibble(state, data[i] >> 4);
			out[i * 2 + 1] = DecodeAdpcmNibble(state, data[i] & 0xF);
		}
		return bytes * 2;
	}

	size_t DecodePcm8(const uint8_t* data, size_t bytes, int16_t* out)
	{
		for (size_t i = 0; i < bytes; i++)
			out[i] = (int16_t)((int8_t)data[i] * 0x100);
		return bytes;
	}

	bool StreamPacer::IsDue(clock::time_point now)
	{
		if (!m_started)
		{
			m_started = true;
			m_next_due = now;
		}
		return now >= m_next_due;
	}

	void StreamPacer::OnSent(clock::time_point now)
	{
		m_sent++;
		m_next_due += m_interval;
		if (now - m_next_due > m_interval * kMaxLagReports)
		{
			// stalled: don't burst the backlog into the remote, restart the schedule now
			m_next_due = now;
			m_resyncs++;
		}
	}
}
