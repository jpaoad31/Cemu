#include "audio/WiimoteSpeakerMixer.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>

namespace WiimoteSpeakerMixer
{
	namespace
	{
		struct Channel
		{
			std::deque<int16_t> fifo;
			uint32_t rate = 6000;
			float volume = 1.0f;
			double frac = 0.0; // position between fifo[0] and fifo[1]
			double level = 0.0; // averaged fifo size, for the rate correction
			bool playing = false;
			Stats stats{};
		};

		std::mutex s_mutex;
		Channel s_channels[kChannels];
		std::atomic<float> s_gain{1.0f};

		size_t MsToSamples(uint32_t ms, uint32_t rate)
		{
			return (size_t)rate * ms / 1000;
		}
	}

	void SetGain(float gain)
	{
		s_gain = std::max(0.0f, gain);
	}

	void Push(size_t channel, const int16_t* samples, size_t count, uint32_t sampleRate, float volume)
	{
		if (channel >= kChannels || sampleRate == 0)
			return;
		std::scoped_lock lock(s_mutex);
		auto& ch = s_channels[channel];
		if (ch.rate != sampleRate)
		{
			ch.fifo.clear();
			ch.frac = 0.0;
			ch.playing = false;
			ch.rate = sampleRate;
		}
		ch.volume = volume;
		ch.fifo.insert(ch.fifo.end(), samples, samples + count);
		ch.stats.pushed += count;
		const size_t max = MsToSamples(kMaxBufferMs, ch.rate);
		if (ch.fifo.size() > max)
		{
			// the producer runs ahead of the output: drop the oldest down to the start level
			const size_t drop = ch.fifo.size() - MsToSamples(kStartBufferMs, ch.rate);
			ch.fifo.erase(ch.fifo.begin(), ch.fifo.begin() + drop);
			ch.stats.trimmed += drop;
		}
	}

	void Clear(size_t channel)
	{
		if (channel >= kChannels)
			return;
		std::scoped_lock lock(s_mutex);
		auto& ch = s_channels[channel];
		ch.fifo.clear();
		ch.frac = 0.0;
		ch.playing = false;
	}

	void ClearAll()
	{
		for (size_t i = 0; i < kChannels; i++)
			Clear(i);
	}

	bool Render(float* out, size_t frames)
	{
		const float gain = s_gain;
		std::scoped_lock lock(s_mutex);
		bool any = false;
		for (auto& ch : s_channels)
		{
			if (!ch.playing)
			{
				if (ch.fifo.size() < std::max<size_t>(2, MsToSamples(kStartBufferMs, ch.rate)))
					continue;
				ch.playing = true;
				ch.level = (double)ch.fifo.size();
			}
			if (!any)
			{
				std::fill(out, out + frames, 0.0f);
				any = true;
			}
			// linear interpolation from the speaker's rate to 48 kHz, slightly faster or slower to
			// keep the buffer near the target level (see the header)
			ch.level += ((double)ch.fifo.size() - ch.level) / kRateAverage;
			const double target = (double)MsToSamples(kStartBufferMs, ch.rate);
			const double max_shift = kMaxRateShift * ch.rate;
			const double shift = std::clamp((ch.level - target) * kRateControl, -max_shift, max_shift);
			const double step = ((double)ch.rate + shift) / kOutputRate;
			const float scale = ch.volume * gain;
			for (size_t i = 0; i < frames; i++)
			{
				if (ch.fifo.size() < 2)
				{
					ch.playing = false;
					ch.stats.underruns++;
					break;
				}
				const float s0 = ch.fifo[0], s1 = ch.fifo[1];
				out[i] += (s0 + (s1 - s0) * (float)ch.frac) * scale;
				ch.frac += step;
				while (ch.frac >= 1.0 && !ch.fifo.empty())
				{
					ch.frac -= 1.0;
					ch.fifo.pop_front();
					ch.stats.played++;
				}
			}
		}
		return any;
	}

	Stats GetStats(size_t channel)
	{
		if (channel >= kChannels)
			return {};
		std::scoped_lock lock(s_mutex);
		Stats stats = s_channels[channel].stats;
		stats.queued = s_channels[channel].fifo.size();
		stats.playing = s_channels[channel].playing;
		return stats;
	}
}
