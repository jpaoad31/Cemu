// padscore: the Wii Remote speaker (WPADControlSpeaker, the stream calls, the volume) and the
// WENC ADPCM encoder.
//
// The usual flow (NintendoWare's nw::snd::RemoteSpeaker, seen in Wii Fit U): WPADControlSpeaker(ON)
// -> callback -> WPADControlSpeaker(PLAY) -> callback; then, from a 6.67 ms alarm, 40 samples from
// AX's remote output (AXRmtGetSamples, see snd_core/ax_rmt.cpp) per playing remote:
// WPADCanSendStreamData -> WENCGetEncodeData(40 samples -> 20 bytes) -> WPADSendStreamData(20 bytes);
// WPADControlSpeaker(OFF) at the end. If WPADSendStreamData fails after WPADCanSendStreamData said
// yes, NintendoWare restarts the speaker, so the two must agree.
//
// Signatures and semantics from Wii Fit U's calls and the Wii SDK's documented behaviour; the
// speaker itself (routing, real remote, system audio) is input/emulated/WiimoteSpeaker.cpp. The
// WENC encoder is Cemu's own (WiimoteSpeakerCodec), matched to Dolphin's decoder; no SDK code.

#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/padscore/padscore.h"
#include "Cafe/OS/libs/coreinit/coreinit.h"
#include "input/emulated/WiimoteSpeaker.h"
#include "input/emulated/WiimoteSpeakerCodec.h"

namespace padscore
{
	using WiimoteSpeaker::Command;
	using WiimoteSpeaker::Result;

	static sint32 SpeakerResultToWPAD(Result result)
	{
		switch (result)
		{
		case Result::Ok: return WPAD_ERR_NONE;
		case Result::NoController: return WPAD_ERR_NO_CONTROLLER;
		case Result::Busy: return WPAD_ERR_BUSY;
		case Result::NoSpeaker: return WPAD_ERR_INVALID;
		}
		return WPAD_ERR_INVALID;
	}

	// command: 0 off, 1 on, 2 mute, 3 unmute, 4 play, 5 on. The callback gets (channel, result)
	// once the command is done; here at once (the real remote's reports are queued in order).
	sint32 WPADControlSpeaker(sint32 channel, uint32 command, MPTR callback)
	{
		sint32 result;
		if (channel < 0 || command > (uint32)Command::On2)
			result = WPAD_ERR_INVALID;
		else
			result = SpeakerResultToWPAD(WiimoteSpeaker::Control((size_t)channel, (Command)command));
		if (callback != MPTR_NULL)
			coreinitAsyncCallback_add(callback, 2, (uint32)channel, (uint32)result);
		return result;
	}

	uint32 WPADIsSpeakerEnabled(sint32 channel)
	{
		return channel >= 0 && WiimoteSpeaker::IsEnabled((size_t)channel) ? TRUE : FALSE;
	}

	uint32 WPADCanSendStreamData(sint32 channel)
	{
		return channel >= 0 && WiimoteSpeaker::CanSendStreamData((size_t)channel) ? TRUE : FALSE;
	}

	// one speaker report: up to 20 bytes (40 ADPCM samples)
	sint32 WPADSendStreamData(sint32 channel, uint8* data, uint32 length)
	{
		if (channel < 0 || !data)
			return WPAD_ERR_INVALID;
		return SpeakerResultToWPAD(WiimoteSpeaker::SendStreamData((size_t)channel, data, length));
	}

	// 0..127, used by the next WPADControlSpeaker(ON)
	uint8 WPADGetSpeakerVolume()
	{
		return WiimoteSpeaker::GetVolume();
	}

	void WPADSetSpeakerVolume(uint8 volume)
	{
		cemuLog_log(LogType::InputAPI, "WiimoteSpeaker: WPADSetSpeakerVolume({})", volume);
		WiimoteSpeaker::SetVolume(volume);
	}

	// The encoder state between calls. Games only clear it (NintendoWare memsets 32 bytes); its
	// contents are this implementation's own.
	struct WENCInfo
	{
		sint32be predictor;
		sint32be step;
		uint8 reserved[24];
	};
	static_assert(sizeof(WENCInfo) == 0x20);

	constexpr uint32 WENC_FLAG_USER_INFO = 1; // continue from `info` (0: start a new stream)

	// 16-bit PCM -> 4-bit ADPCM, (samples + 1) / 2 bytes, first sample in the high nibble.
	// Returns the number of samples encoded.
	sint32 WENCGetEncodeData(WENCInfo* info, uint32 flag, sint16be* pcm, sint32 samples, uint8* out)
	{
		if (!info || !pcm || !out || samples <= 0)
			return 0;
		WiimoteSpeakerCodec::AdpcmState state;
		if (flag & WENC_FLAG_USER_INFO)
		{
			state.predictor = info->predictor;
			state.step = std::clamp<sint32>(info->step, 127, 24576);
		}
		int16_t block[256];
		for (sint32 done = 0; done < samples;)
		{
			// blocks of an even number of samples, so each starts on a byte
			const sint32 n = std::min<sint32>(samples - done, (sint32)std::size(block));
			for (sint32 i = 0; i < n; i++)
				block[i] = pcm[done + i];
			WiimoteSpeakerCodec::EncodeAdpcm(state, block, (size_t)n, out + done / 2);
			done += n;
		}
		info->predictor = state.predictor;
		info->step = state.step;
		return samples;
	}

	void speaker_load()
	{
		cafeExportRegister("padscore", WPADControlSpeaker, LogType::InputAPI);
		cafeExportRegister("padscore", WPADIsSpeakerEnabled, LogType::InputAPI);
		cafeExportRegister("padscore", WPADCanSendStreamData, LogType::Placeholder); // every 6.67 ms
		cafeExportRegister("padscore", WPADSendStreamData, LogType::Placeholder);   // every 6.67 ms, logs its own summary
		cafeExportRegister("padscore", WPADGetSpeakerVolume, LogType::InputAPI);
		cafeExportRegister("padscore", WPADSetSpeakerVolume, LogType::InputAPI);
		cafeExportRegister("padscore", WENCGetEncodeData, LogType::Placeholder);
	}

	void speaker_reset()
	{
		WiimoteSpeaker::Reset();
	}
}
