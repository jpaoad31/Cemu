// Standalone check of WiimoteSpeakerCodec (not part of the CMake build: Cemu has no test target).
// Also checks the system-audio mixer (src/audio/WiimoteSpeakerMixer). Build and run from the repository root:
//   clang++ -std=c++20 -Wall -I src src/input/emulated/WiimoteSpeakerCodecTest.cpp src/input/emulated/WiimoteSpeakerCodec.cpp src/audio/WiimoteSpeakerMixer.cpp -o /tmp/spktest && /tmp/spktest
#include "input/emulated/WiimoteSpeakerCodec.h"
#include "audio/WiimoteSpeakerMixer.h"
#include <cmath>
#include <cstdio>
#include <vector>

using namespace WiimoteSpeakerCodec;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

// Reference decoder: Dolphin's emulated speaker (Source/Core/Core/HW/WiimoteEmu/Speaker.cpp,
// GPL-2.0-or-later), kept as written there, to check the codec against it
namespace dolphin
{
	struct ADPCMState { int32_t predictor, step; };
	static const int32_t yamaha_difflookup[] = {1, 3, 5, 7, 9, 11, 13, 15, -1, -3, -5, -7, -9, -11, -13, -15};
	static const int32_t yamaha_indexscale[] = {230, 230, 230, 230, 307, 409, 512, 614, 230, 230, 230, 230, 307, 409, 512, 614};
	static int16_t av_clip16(int32_t a) { if ((a + 32768) & ~65535) return (a >> 31) ^ 32767; else return a; }
	static int32_t av_clip(int32_t a, int32_t amin, int32_t amax) { if (a < amin) return amin; else if (a > amax) return amax; else return a; }
	static int16_t adpcm_yamaha_expand_nibble(ADPCMState& s, uint8_t nibble)
	{
		s.predictor += (s.step * yamaha_difflookup[nibble]) / 8;
		s.predictor = av_clip16(s.predictor);
		s.step = (s.step * yamaha_indexscale[nibble]) >> 8;
		s.step = av_clip(s.step, 127, 24576);
		return s.predictor;
	}
	static std::vector<int16_t> decode(const std::vector<uint8_t>& data)
	{
		ADPCMState s{0, 127}; // SpeakerLogic::Reset
		std::vector<int16_t> out;
		for (uint8_t b : data)
		{
			out.push_back(adpcm_yamaha_expand_nibble(s, (b >> 4) & 0xf));
			out.push_back(adpcm_yamaha_expand_nibble(s, b & 0xf));
		}
		return out;
	}
}

static double snr_db(const std::vector<int16_t>& ref, const std::vector<int16_t>& test, size_t skip)
{
	double sig = 0, noise = 0;
	for (size_t i = skip; i < ref.size() && i < test.size(); i++)
	{
		sig += (double)ref[i] * ref[i];
		const double d = (double)ref[i] - test[i];
		noise += d * d;
	}
	return noise == 0 ? 999.0 : 10.0 * std::log10(sig / noise);
}

static std::vector<int16_t> tone(double freq, double amplitude, double rate, size_t n)
{
	std::vector<int16_t> pcm(n);
	for (size_t i = 0; i < n; i++)
		pcm[i] = (int16_t)std::lround(amplitude * std::sin(2.0 * M_PI * freq * i / rate));
	return pcm;
}

static void test_codec()
{
	const Config cfg; // ADPCM, 0x07D0
	CHECK(cfg.SampleRate() == 6000);
	CHECK(cfg.SamplesPerReport() == 40);
	CHECK(cfg.ReportInterval().count() == 6666666);

	uint8_t reg[7];
	BuildConfigRegister(cfg, reg);
	const uint8_t expected[7] = {0x00, 0x00, 0xD0, 0x07, kDefaultVolume, 0x0C, 0x0E};
	for (int i = 0; i < 7; i++)
		CHECK(reg[i] == expected[i]);

	// encode tones in 40-sample blocks (as a game does it, one report at a time), decode with
	// Dolphin's decoder and with ours
	struct Case { double freq, amplitude; double min_snr; };
	const Case cases[] = {{440, 12000, 15}, {1000, 20000, 12}, {250, 3000, 15}, {2000, 16000, 8}};
	for (const auto& c : cases)
	{
		const auto pcm = tone(c.freq, c.amplitude, 6000, 6000);
		AdpcmState enc;
		std::vector<uint8_t> adpcm;
		for (size_t off = 0; off < pcm.size(); off += 40)
		{
			uint8_t block[20];
			const size_t n = EncodeAdpcm(enc, pcm.data() + off, 40, block);
			CHECK(n == 20);
			adpcm.insert(adpcm.end(), block, block + n);
		}
		CHECK(adpcm.size() == 3000);
		const auto ref = dolphin::decode(adpcm);
		AdpcmState dec;
		std::vector<int16_t> ours(adpcm.size() * 2);
		DecodeAdpcm(dec, adpcm.data(), adpcm.size(), ours.data());
		CHECK(ours == ref);                  // our decoder = Dolphin's
		CHECK(enc.predictor == dec.predictor && enc.step == dec.step); // encoder tracks the decoder
		const double snr = snr_db(pcm, ref, 200);
		printf("tone %4.0f Hz amplitude %5.0f: %zu bytes, SNR %.1f dB (Dolphin's decoder)\n", c.freq, c.amplitude, adpcm.size(), snr);
		CHECK(snr >= c.min_snr);
	}

	// silence stays silent once the step has decayed
	{
		std::vector<int16_t> zero(400, 0);
		AdpcmState enc;
		std::vector<uint8_t> adpcm(200);
		EncodeAdpcm(enc, zero.data(), zero.size(), adpcm.data());
		const auto ref = dolphin::decode(adpcm);
		int peak = 0;
		for (size_t i = 0; i < ref.size(); i++)
			peak = std::max(peak, std::abs((int)ref[i]));
		printf("silence: peak %d\n", peak);
		CHECK(peak <= 16);
	}
	// full-scale square wave: clipping at the 16-bit limits doesn't break tracking
	{
		std::vector<int16_t> sq(2000);
		for (size_t i = 0; i < sq.size(); i++)
			sq[i] = ((i / 15) & 1) ? 32767 : -32768;
		AdpcmState enc;
		std::vector<uint8_t> adpcm(1000);
		EncodeAdpcm(enc, sq.data(), sq.size(), adpcm.data());
		const auto ref = dolphin::decode(adpcm);
		const double snr = snr_db(sq, ref, 100);
		printf("full-scale square: SNR %.1f dB\n", snr);
		CHECK(snr >= 6);
	}
	// odd sample count: the last byte has only its high nibble
	{
		const int16_t pcm[3] = {1000, 2000, 3000};
		uint8_t out[2] = {0xFF, 0xFF};
		AdpcmState enc;
		CHECK(EncodeAdpcm(enc, pcm, 3, out) == 2);
		CHECK((out[1] & 0x0F) == 0);
	}
	// 8-bit PCM
	{
		const uint8_t data[4] = {0x00, 0x7F, 0x80, 0xFF};
		int16_t out[4];
		CHECK(DecodePcm8(data, 4, out) == 4);
		CHECK(out[0] == 0 && out[1] == 0x7F00 && out[2] == -0x8000 && out[3] == -0x100);
		Config pcm8{kFormatPcm8, 0x0FA0, 0xFF};
		CHECK(pcm8.SampleRate() == 6000 && pcm8.SamplesPerReport() == 20 && pcm8.Gain() == 1.0f);
	}
	CHECK((Config{kFormatAdpcm, 0x07D0, 0x7F}.Gain() == 1.0f));
	CHECK(std::fabs(Config{}.Gain() - 89.0f / 127.0f) < 1e-6);
}

// The provider's writer thread checks the pacer about once per millisecond (1 ms sleep plus
// scheduling jitter). Simulate that and look at the cadence.
struct PaceResult { uint64_t sent; double per_second; double max_gap_ms; double mean_gap_ms; uint32_t resyncs; };

static PaceResult simulate(const Config& cfg, double seconds, double tick_ms, double stall_at_ms = -1, double stall_ms = 0)
{
	using clock = StreamPacer::clock;
	StreamPacer pacer;
	pacer.SetInterval(cfg.ReportInterval());
	const auto t0 = clock::time_point{} + std::chrono::hours(1);
	double t = 0, last_send = -1, max_gap = 0, sum_gap = 0;
	uint64_t gaps = 0;
	bool stalled = false;
	uint32_t tick = 0;
	while (t < seconds * 1000.0)
	{
		if (stall_at_ms >= 0 && !stalled && t >= stall_at_ms)
		{
			t += stall_ms; // the thread didn't run
			stalled = true;
		}
		const auto now = t0 + std::chrono::nanoseconds((int64_t)(t * 1e6));
		// at most one report per tick, like the writer thread (one write per device per pass)
		if (pacer.IsDue(now))
		{
			pacer.OnSent(now);
			if (last_send >= 0)
			{
				const double gap = t - last_send;
				if (!(stall_at_ms >= 0 && last_send < stall_at_ms + stall_ms && t >= stall_at_ms && gap > stall_ms))
					max_gap = std::max(max_gap, gap);
				sum_gap += gap;
				gaps++;
			}
			last_send = t;
		}
		// tick with a little jitter: tick_ms +- 0.3 ms
		t += tick_ms + 0.3 * std::sin(tick++ * 1.7);
	}
	return {pacer.Sent(), pacer.Sent() / seconds, max_gap, gaps ? sum_gap / gaps : 0, pacer.Resyncs()};
}

static void test_pacing()
{
	struct Case { Config cfg; const char* name; double expected_per_second; };
	const Case cases[] = {
		{Config{kFormatAdpcm, 0x07D0, 0x40}, "ADPCM 0x07D0 (6000 Hz, the Wii/Wii U default)", 150.0},
		{Config{kFormatAdpcm, 0x0FA0, 0x40}, "ADPCM 0x0FA0 (3000 Hz)", 75.0},
		{Config{kFormatAdpcm, 0x0BB8, 0x40}, "ADPCM 0x0BB8 (4000 Hz)", 100.0},
		{Config{kFormatPcm8, 0x0FA0, 0x40}, "PCM8  0x0FA0 (6000 Hz)", 300.0},
		{Config{kFormatPcm8, 0x1770, 0x40}, "PCM8  0x1770 (4000 Hz)", 200.0},
	};
	for (const auto& c : cases)
	{
		const auto r = simulate(c.cfg, 10.0, 1.0);
		printf("%-46s interval %.3f ms: %llu reports in 10 s = %.1f/s (expected %.1f), mean gap %.3f ms, max gap %.2f ms, resyncs %u\n",
			c.name, c.cfg.ReportInterval().count() / 1e6, (unsigned long long)r.sent, r.per_second, c.expected_per_second, r.mean_gap_ms, r.max_gap_ms, r.resyncs);
		CHECK(std::fabs(r.per_second - c.expected_per_second) <= 0.5);
		CHECK(r.resyncs == 0);
		// the cadence holds on average; single gaps are at most one interval + one tick late
		CHECK(r.max_gap_ms <= c.cfg.ReportInterval().count() / 1e6 + 2.5);
	}
	// a 100 ms stall of the sender: no burst afterwards (one resync), the rate continues
	{
		const Config cfg;
		const auto r = simulate(cfg, 10.0, 1.0, 3000.0, 100.0);
		printf("100 ms stall at 3 s: %llu reports in 10 s, resyncs %u\n", (unsigned long long)r.sent, r.resyncs);
		CHECK(r.resyncs == 1);
		CHECK(r.sent >= 1480 && r.sent <= 1490); // 1500 minus the ~15 reports lost in the stall
	}
	// a short hiccup (10 ms) is caught up without a resync
	{
		const Config cfg;
		const auto r = simulate(cfg, 10.0, 1.0, 3000.0, 10.0);
		printf("10 ms hiccup at 3 s: %llu reports in 10 s, resyncs %u\n", (unsigned long long)r.sent, r.resyncs);
		CHECK(r.resyncs == 0);
		CHECK(r.sent >= 1499 && r.sent <= 1501);
	}
}

// The game's stream (ADPCM, 40 samples per 6.67 ms, in bursts) through the decoder and the mixer
// at 48 kHz, 144 frames per 3 ms AX frame. The output must be the tone, without gaps.
static void test_mixer()
{
	namespace M = WiimoteSpeakerMixer;
	M::ClearAll();
	M::SetGain(1.0f);
	const double freq = 500.0;
	const auto pcm = tone(freq, 10000, 6000, 6000 * 4); // 4 s
	AdpcmState enc, dec;
	size_t in_pos = 0;
	std::vector<float> out;
	float frame[144];
	// time in 1 ms steps: the game sends 2 reports every 13.33 ms (a burst), AX renders every 3 ms
	double next_send = 0, next_render = 0;
	for (double t = 0; t < 3900; t += 0.5)
	{
		while (t >= next_send && in_pos + 80 <= pcm.size())
		{
			for (int r = 0; r < 2; r++)
			{
				uint8_t block[20];
				EncodeAdpcm(enc, pcm.data() + in_pos, 40, block);
				int16_t decoded[40];
				DecodeAdpcm(dec, block, 20, decoded);
				M::Push(0, decoded, 40, 6000, 1.0f);
				in_pos += 40;
			}
			next_send += 80.0 / 6.0; // 13.33 ms
		}
		if (t >= next_render)
		{
			if (M::Render(frame, 144))
				out.insert(out.end(), frame, frame + 144);
			else
				out.insert(out.end(), 144, 0.0f);
			next_render += 3.0;
		}
	}
	const auto st = M::GetStats(0);
	// skip the start-up buffer (30 ms), then compare against a 500 Hz sine: count zero crossings
	// and look for silent gaps
	size_t crossings = 0, longest_quiet = 0, quiet = 0;
	double energy = 0;
	const size_t begin = 48 * 60, end = out.size();
	for (size_t i = begin + 1; i < end; i++)
	{
		if ((out[i - 1] < 0) != (out[i] < 0))
			crossings++;
		quiet = std::fabs(out[i]) < 50 ? quiet + 1 : 0;
		longest_quiet = std::max(longest_quiet, quiet);
		energy += (double)out[i] * out[i];
	}
	const double seconds = (end - begin) / 48000.0;
	const double measured = crossings / 2.0 / seconds;
	const double rms = std::sqrt(energy / (end - begin));
	printf("mixer: %.2f s rendered, %.1f Hz measured (500 expected), RMS %.0f (7071 expected), longest quiet run %zu samples, "
		"pushed %llu, played %llu, underruns %llu, trimmed %llu, queued %zu\n",
		seconds, measured, rms, longest_quiet, (unsigned long long)st.pushed, (unsigned long long)st.played,
		(unsigned long long)st.underruns, (unsigned long long)st.trimmed, st.queued);
	CHECK(std::fabs(measured - freq) < 3);
	CHECK(rms > 6000 && rms < 8000);
	CHECK(longest_quiet < 20); // no gaps (a sine is near zero for only a few samples)
	CHECK(st.underruns == 0 && st.trimmed == 0);

	// the producer stops: the channel drains and goes quiet, once
	for (int i = 0; i < 20; i++)
		M::Render(frame, 144);
	CHECK(!M::Render(frame, 144));
	CHECK(M::GetStats(0).underruns == 1);

	// too much at once is trimmed to the start level
	std::vector<int16_t> lots(6000, 1000);
	M::Push(1, lots.data(), lots.size(), 6000, 0.5f);
	CHECK(M::GetStats(1).queued == 180 && M::GetStats(1).trimmed == 6000 - 180);
	CHECK(M::Render(frame, 144));
	CHECK(std::fabs(frame[10] - 500.0f) < 1.0f); // volume 0.5
	M::ClearAll();
}

int main()
{
	test_codec();
	test_pacing();
	test_mixer();
	printf(fails ? "%d FAILED\n" : "all passed\n", fails);
	return fails ? 1 : 0;
}
