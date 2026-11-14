// AX remote (Wii Remote speaker) output: AX_DEV_RMT
//
// Voices can be routed to the four Wii Remote speakers (AXSetVoiceRmtOn + AXSetVoiceDeviceMix
// with AX_DEV_RMT). AX mixes them into one 6 kHz mono stream per remote, 18 samples per 3 ms
// frame, and keeps them in a small buffer. The game takes them out with AXRmtGetSamplesLeft /
// AXRmtGetSamples / AXRmtAdvancePtr, encodes them (WENCGetEncodeData) and sends them to the
// remote (WPADSendStreamData). NintendoWare's nw::snd::RemoteSpeakerManager does exactly that from
// a 6.67 ms alarm: when 40 or more samples are left, it reads 40 for each playing remote and then
// advances by 40 (seen in Wii Fit U's code).
//
// Only the main bus is mixed (NintendoWare sets only the main bus volume for remotes). The remote
// filter (AXSetVoiceRmtIIR) is accepted but not applied: it only shapes the sound for the small
// speaker. Behaviour from the API's use in Wii Fit U; no SDK code was used.

#include "Cafe/OS/libs/snd_core/ax.h"
#include "Cafe/OS/libs/snd_core/ax_internal.h"
#include "Cafe/OS/common/OSCommon.h"

namespace snd_core
{
	constexpr sint32 AX_RMT_SAMPLES_PER_FRAME = 18; // 6 kHz, 3 ms
	constexpr sint32 AX_RMT_BUFFER_SAMPLES = 32 * AX_RMT_SAMPLES_PER_FRAME; // ~96 ms

	struct
	{
		std::mutex mutex;
		// mix of the current frame per remote, at the renderer rate (s16 << 8, like the other buses)
		float mix[AX_RMT_DEVICE_COUNT][AX_SAMPLES_MAX];
		bool mixed[AX_RMT_DEVICE_COUNT];
		// 6 kHz ring per remote; all four advance together
		sint16 ring[AX_RMT_DEVICE_COUNT][AX_RMT_BUFFER_SAMPLES];
		uint32 writePos = 0; // total samples written
		uint32 readPos = 0;  // total samples consumed
		uint32 overruns = 0;
		bool loggedFirstVoice = false;
		bool loggedAux = false;
	}s_rmt;

	void AXRmt_Init()
	{
		std::scoped_lock lock(s_rmt.mutex);
		memset(s_rmt.mix, 0, sizeof(s_rmt.mix));
		memset(s_rmt.mixed, 0, sizeof(s_rmt.mixed));
		memset(s_rmt.ring, 0, sizeof(s_rmt.ring));
		s_rmt.writePos = 0;
		s_rmt.readPos = 0;
		s_rmt.overruns = 0;
		s_rmt.loggedFirstVoice = false;
		s_rmt.loggedAux = false;
	}

	// called for every processed voice, after its samples are decoded and enveloped
	void AXRmt_MixVoice(AXVPBInternal_t* internalShadowCopy, float* sampleData, sint32 sampleCount)
	{
		if (internalShadowCopy->reserved148_voiceRmtOn == 0)
			return;
		for (sint32 remote = 0; remote < AX_RMT_DEVICE_COUNT; remote++)
		{
			const uint16* mask = internalShadowCopy->deviceMixMaskRMT + remote * AX_BUS_COUNT;
			if (!s_rmt.loggedAux)
			{
				for (sint32 bus = 1; bus < AX_BUS_COUNT; bus++)
				{
					if (mask[bus] != 0)
					{
						s_rmt.loggedAux = true;
						cemuLog_log(LogType::InputAPI, "WiimoteSpeaker: AX remote output: a voice uses AUX bus {} of remote {}, only the main bus is mixed", bus, remote);
					}
				}
			}
			if (mask[0] == 0)
				continue;
			AXCHMIX_DEPR* mix = internalShadowCopy->deviceMixRMT + remote * AX_BUS_COUNT;
			AXVoiceMix_MergeInto(sampleData, s_rmt.mix[remote], sampleCount, mix, _swapEndianS16(mix->delta));
			s_rmt.mixed[remote] = true;
			if (!s_rmt.loggedFirstVoice)
			{
				s_rmt.loggedFirstVoice = true;
				cemuLog_log(LogType::InputAPI, "WiimoteSpeaker: AX remote output: first voice mixed to remote {} (volume 0x{:04x})", remote, _swapEndianU16(mix->vol));
			}
		}
	}

	// Test hook: CEMU_AX_RMT_TEST_TONE=<Hz> adds a tone to every remote's output, 2 s on / 2 s off,
	// as if the game played a sound on the remotes. With a game that has the speaker playing, this
	// runs the game's own stream code (encoder, WPADSendStreamData) without needing a remote sound.
	static sint32 AXRmt_TestTone(uint64 sampleIndex)
	{
		static const double s_freq = [] {
			const char* env = getenv("CEMU_AX_RMT_TEST_TONE");
			return env ? atof(env) : 0.0;
		}();
		if (s_freq <= 0.0 || (sampleIndex / (2 * 6000)) % 2 != 0)
			return 0;
		return (sint32)(8000.0 * std::sin(2.0 * 3.14159265358979 * s_freq * (double)sampleIndex / 6000.0));
	}

	// end of a frame: downsample each remote's mix to 18 samples (6 kHz) into the ring
	void AXRmt_ProcessFrame()
	{
		const sint32 inputCount = AXGetInputSamplesPerFrame();
		std::scoped_lock lock(s_rmt.mutex);
		if (s_rmt.writePos - s_rmt.readPos + AX_RMT_SAMPLES_PER_FRAME > AX_RMT_BUFFER_SAMPLES)
		{
			// nobody reads (no remote speaker in use): drop the oldest samples
			s_rmt.readPos = s_rmt.writePos + AX_RMT_SAMPLES_PER_FRAME - AX_RMT_BUFFER_SAMPLES;
			s_rmt.overruns++;
		}
		for (sint32 remote = 0; remote < AX_RMT_DEVICE_COUNT; remote++)
		{
			for (sint32 k = 0; k < AX_RMT_SAMPLES_PER_FRAME; k++)
			{
				sint32 out = AXRmt_TestTone((uint64)s_rmt.writePos + k);
				if (s_rmt.mixed[remote])
				{
					// box filter over the input samples that fall into this output sample
					const sint32 begin = k * inputCount / AX_RMT_SAMPLES_PER_FRAME;
					const sint32 end = (k + 1) * inputCount / AX_RMT_SAMPLES_PER_FRAME;
					float sum = 0.0f;
					for (sint32 i = begin; i < end; i++)
						sum += s_rmt.mix[remote][i];
					out += (sint32)(sum / (float)std::max(1, end - begin) / 256.0f);
				}
				s_rmt.ring[remote][(s_rmt.writePos + k) % AX_RMT_BUFFER_SAMPLES] = (sint16)std::clamp<sint32>(out, -32768, 32767);
			}
		}
		s_rmt.writePos += AX_RMT_SAMPLES_PER_FRAME;
		memset(s_rmt.mix, 0, sizeof(s_rmt.mix));
		memset(s_rmt.mixed, 0, sizeof(s_rmt.mixed));
	}

	// samples each remote's buffer holds
	sint32 AXRmtGetSamplesLeft()
	{
		std::scoped_lock lock(s_rmt.mutex);
		return (sint32)(s_rmt.writePos - s_rmt.readPos);
	}

	// copies up to `count` samples of one remote without consuming them; returns how many
	sint32 AXRmtGetSamples(sint32 remote, sint16be* buffer, sint32 count)
	{
		if (remote < 0 || remote >= AX_RMT_DEVICE_COUNT || !buffer || count <= 0)
			return 0;
		std::scoped_lock lock(s_rmt.mutex);
		const sint32 n = std::min(count, (sint32)(s_rmt.writePos - s_rmt.readPos));
		for (sint32 i = 0; i < n; i++)
			buffer[i] = s_rmt.ring[remote][(s_rmt.readPos + i) % AX_RMT_BUFFER_SAMPLES];
		return n;
	}

	// consumes `count` samples of all remotes; returns how many
	sint32 AXRmtAdvancePtr(sint32 count)
	{
		if (count <= 0)
			return 0;
		std::scoped_lock lock(s_rmt.mutex);
		const sint32 n = std::min(count, (sint32)(s_rmt.writePos - s_rmt.readPos));
		s_rmt.readPos += n;
		return n;
	}

}
