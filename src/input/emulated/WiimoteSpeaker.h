#pragma once

// The Wii Remote speaker as a game sees it, per WPAD channel (the HLE side is in
// Cafe/OS/libs/padscore/padscore_speaker.cpp).
//
// A game turns the speaker on (WPADControlSpeaker ON: enable, configure, unmute), starts it
// (PLAY), then streams 4-bit ADPCM (WPADSendStreamData, 20 bytes = 40 samples every 6.67 ms) and
// turns it off. Where the sound goes is the speaker output setting:
//   - Remote: the real Wii Remote's own speaker (any Wiimote backend: hidapi, IOBluetooth, the
//     Wii BT Bridge). The setup and the stream reports are sent to it, paced at the stream rate.
//   - SystemAudio: decoded here and mixed into Cemu's TV output (never the
//     GamePad, which has its own audio stream on a real Wii U) (WiimoteSpeakerMixer).
//   - Both, or Off (the game still sees a working speaker; the sound is dropped).
// Real Wii Remotes have the setting in their controller settings (saved in the controller
// profile, default Remote). Emulated Wii Remotes (keyboard, gamepads) have no speaker of their
// own and use the global setting <Input><WiimoteSpeaker emulated="..."/> (default SystemAudio).

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace WiimoteSpeaker
{
	enum class Output
	{
		Remote,      // the real remote's speaker
		SystemAudio, // Cemu's audio output
		Both,
		Off,
	};
	std::string_view OutputToString(Output output); // for the config: remote, system, both, off
	Output OutputFromString(std::string_view text, Output fallback);

	// WPADControlSpeaker commands
	enum class Command : uint32_t
	{
		Off = 0,
		On = 1,
		Mute = 2,
		Unmute = 3,
		Play = 4,
		On2 = 5, // the same as On
	};

	enum class Result
	{
		Ok,
		NoController, // nothing on this channel
		NoSpeaker,    // a device without a speaker (Balance Board, Pro Controller)
		Busy,         // the real remote's queue is full
	};

	// volume for the next speaker On (0..127, WPADGetSpeakerVolume / WPADSetSpeakerVolume)
	uint8_t GetVolume();
	void SetVolume(uint8_t volume);

	Result Control(size_t channel, Command command);
	bool IsEnabled(size_t channel); // on (WPADInfo.speaker)
	bool CanSendStreamData(size_t channel);
	Result SendStreamData(size_t channel, const uint8_t* data, size_t length);

	// the game ends (or the emulator stops): forget all state
	void Reset();
}
