#include "input/emulated/WiimoteSpeaker.h"

#include "input/emulated/WiimoteSpeakerCodec.h"
#include "input/emulated/WPADController.h"
#include "input/InputManager.h"
#include "audio/WiimoteSpeakerMixer.h"
#include "config/CemuConfig.h"
#ifdef SUPPORTS_WIIMOTE
#include "input/api/Wiimote/NativeWiimoteController.h"
#endif

namespace WiimoteSpeaker
{
	using WiimoteSpeakerCodec::Config;

	std::string_view OutputToString(Output output)
	{
		switch (output)
		{
		case Output::Remote: return "remote";
		case Output::SystemAudio: return "system";
		case Output::Both: return "both";
		case Output::Off: return "off";
		}
		return "remote";
	}

	Output OutputFromString(std::string_view text, Output fallback)
	{
		if (text == "remote")
			return Output::Remote;
		if (text == "system")
			return Output::SystemAudio;
		if (text == "both")
			return Output::Both;
		if (text == "off")
			return Output::Off;
		return fallback;
	}

	namespace
	{
		constexpr size_t kChannels = InputManager::kMaxWPADControllers;
		constexpr uint64 kStatsReports = 1500; // log every ~10 s of 6 kHz ADPCM

		struct Channel
		{
			bool enabled = false; // On .. Off
			bool muted = false;
			bool playing = false;
			Config config{};
			WiimoteSpeakerCodec::AdpcmState decoder{};
			sint32 real_index = -1; // Wiimote device that got the setup (speaker on)
			uint32 real_connection = 0; // ... and its connection (a reconnected remote needs it again)

			// statistics of the current stream
			uint64 reports = 0, real_dropped = 0;
			uint64 window_reports = 0;
			std::chrono::steady_clock::time_point window_start{};
			std::string last_route;
		};

		std::mutex s_mutex;
		Channel s_channels[kChannels];
		std::atomic<uint8_t> s_volume{WiimoteSpeakerCodec::kDefaultVolume};

		const char* CommandName(Command command)
		{
			switch (command)
			{
			case Command::Off: return "off";
			case Command::On: return "on";
			case Command::Mute: return "mute";
			case Command::Unmute: return "unmute";
			case Command::Play: return "play";
			case Command::On2: return "on (5)";
			}
			return "?";
		}

		// where this channel's speaker sound goes now
		struct Route
		{
#ifdef SUPPORTS_WIIMOTE
			std::shared_ptr<NativeWiimoteController> real;
#endif
			sint32 real_index = -1;
			uint32 real_connection = 0;
			bool to_real = false;
			bool to_system = false;
			std::string describe() const
			{
				std::string text;
				if (to_real)
					text = fmt::format("real Wii Remote (Wiimote device {})", real_index);
				if (to_system)
					text += fmt::format("{}system audio (TV)", text.empty() ? "" : " + ");
				return text.empty() ? "off" : text;
			}
		};

		Route Resolve(const WPADController& controller, size_t channel)
		{
			Route route;
			const auto& config = GetConfig();
			Output output = OutputFromString(config.wiimote_speaker.emulated.GetValue(), Output::SystemAudio);
			if (output != Output::Off)
				output = Output::SystemAudio; // emulated remotes: system audio or nothing
#ifdef SUPPORTS_WIIMOTE
			if (auto native = controller.get_native_wiimote(); native && !native->is_balance_board())
			{
				output = native->get_speaker_output();
				if ((output == Output::Remote || output == Output::Both) && native->is_connected())
				{
					route.real = native;
					route.real_index = (sint32)native->get_index();
					route.real_connection = native->get_connection_id();
					route.to_real = true;
				}
			}
#endif
			route.to_system = (output == Output::SystemAudio || output == Output::Both) && channel < WiimoteSpeakerMixer::kChannels;

			WiimoteSpeakerMixer::SetGain(std::clamp(config.wiimote_speaker.volume.GetValue(), 0, 400) / 100.0f);
			return route;
		}

		bool HasSpeaker(const WPADController& controller)
		{
			const auto type = controller.get_device_type();
			return type != kWAPDevBalanceChecker && type != kWAPDevURCC;
		}

		void LogStreamStats(size_t channel, Channel& ch, const char* why)
		{
			if (ch.reports == 0)
				return;
			const auto mixer = channel < WiimoteSpeakerMixer::kChannels ? WiimoteSpeakerMixer::GetStats(channel) : WiimoteSpeakerMixer::Stats{};
			cemuLog_log(LogType::InputAPI, "WiimoteSpeaker: channel {}: {}: {} reports from the game, {} not taken by the real remote; system audio: {} samples pushed, {} played, {} trimmed, {} underruns, {} queued",
				channel, why, ch.reports, ch.real_dropped, mixer.pushed, mixer.played, mixer.trimmed, mixer.underruns, mixer.queued);
		}

#ifdef SUPPORTS_WIIMOTE
		// the remote behind the route didn't get the setup yet (it connected later, or another
		// remote was assigned): set it up now, so its stream matches the game's state
		void EnsureRealSetup(Channel& ch, const Route& route, size_t channel)
		{
			if (!route.to_real || !ch.enabled || (ch.real_index == route.real_index && ch.real_connection == route.real_connection))
				return;
			cemuLog_log(LogType::InputAPI, "WiimoteSpeaker: channel {}: setting up Wiimote device {} (speaker already on in the game; the remote {})", channel, route.real_index,
				ch.real_index == route.real_index ? "reconnected" : "was assigned now");
			route.real->speaker_on(ch.config);
			if (ch.muted)
				route.real->speaker_mute(true);
			if (ch.playing)
				route.real->speaker_play();
			ch.real_index = route.real_index;
			ch.real_connection = route.real_connection;
		}
#endif
	}

	uint8_t GetVolume()
	{
		return s_volume;
	}

	void SetVolume(uint8_t volume)
	{
		s_volume = std::min<uint8_t>(volume, WiimoteSpeakerCodec::kMaxVolumeAdpcm);
	}

	Result Control(size_t channel, Command command)
	{
		const auto controller = channel < kChannels ? InputManager::instance().get_wpad_controller(channel) : nullptr;
		if (!controller)
			return Result::NoController;
		if (!HasSpeaker(*controller))
		{
			cemuLog_log(LogType::InputAPI, "WiimoteSpeaker: channel {}: {}: this device has no speaker", channel, CommandName(command));
			return Result::NoSpeaker;
		}

		const Route route = Resolve(*controller, channel);
		std::scoped_lock lock(s_mutex);
		auto& ch = s_channels[channel];
		switch (command)
		{
		case Command::On:
		case Command::On2:
			ch.enabled = true;
			ch.muted = false;
			ch.playing = false;
			ch.config = Config{WiimoteSpeakerCodec::kFormatAdpcm, WiimoteSpeakerCodec::kDefaultRateValue, s_volume};
			ch.decoder = {};
			ch.reports = ch.real_dropped = ch.window_reports = 0;
			ch.window_start = {};
			if (channel < WiimoteSpeakerMixer::kChannels)
				WiimoteSpeakerMixer::Clear(channel);
#ifdef SUPPORTS_WIIMOTE
			ch.real_index = -1;
			if (route.to_real)
			{
				route.real->speaker_on(ch.config);
				ch.real_index = route.real_index;
				ch.real_connection = route.real_connection;
			}
#endif
			break;
		case Command::Off:
			LogStreamStats(channel, ch, "speaker off");
			ch.enabled = ch.muted = ch.playing = false;
			if (channel < WiimoteSpeakerMixer::kChannels)
				WiimoteSpeakerMixer::Clear(channel);
#ifdef SUPPORTS_WIIMOTE
			if (route.to_real && ch.real_index == route.real_index)
				route.real->speaker_off();
#endif
			ch.real_index = -1;
			break;
		case Command::Mute:
		case Command::Unmute:
			ch.muted = command == Command::Mute;
			if (ch.muted && channel < WiimoteSpeakerMixer::kChannels)
				WiimoteSpeakerMixer::Clear(channel);
#ifdef SUPPORTS_WIIMOTE
			EnsureRealSetup(ch, route, channel);
			if (route.to_real)
				route.real->speaker_mute(ch.muted);
#endif
			break;
		case Command::Play:
			ch.playing = true;
			ch.decoder = {}; // a new stream starts with a fresh encoder (WENC first block)
#ifdef SUPPORTS_WIIMOTE
			EnsureRealSetup(ch, route, channel);
			if (route.to_real)
				route.real->speaker_play();
#endif
			break;
		}
		const std::string where = route.describe();
		cemuLog_log(LogType::InputAPI, "WiimoteSpeaker: channel {}: WPADControlSpeaker({}){}{}", channel, CommandName(command),
			command == Command::On || command == Command::On2 ? fmt::format(": ADPCM, {} Hz, volume {}", ch.config.SampleRate(), ch.config.volume) : "",
			fmt::format(", output: {}", where));
		ch.last_route = where;
		return Result::Ok;
	}

	bool IsEnabled(size_t channel)
	{
		if (channel >= kChannels)
			return false;
		std::scoped_lock lock(s_mutex);
		return s_channels[channel].enabled;
	}

	bool CanSendStreamData(size_t channel)
	{
		const auto controller = channel < kChannels ? InputManager::instance().get_wpad_controller(channel) : nullptr;
		if (!controller || !HasSpeaker(*controller))
			return false;
#ifdef SUPPORTS_WIIMOTE
		const Route route = Resolve(*controller, channel);
		if (route.to_real)
			return route.real->speaker_can_send();
#endif
		return true;
	}

	Result SendStreamData(size_t channel, const uint8_t* data, size_t length)
	{
		const auto controller = channel < kChannels ? InputManager::instance().get_wpad_controller(channel) : nullptr;
		if (!controller)
			return Result::NoController;
		if (!HasSpeaker(*controller))
			return Result::NoSpeaker;
		length = std::min(length, WiimoteSpeakerCodec::kReportDataSize);

		const Route route = Resolve(*controller, channel);
		std::scoped_lock lock(s_mutex);
		auto& ch = s_channels[channel];
		const auto now = std::chrono::steady_clock::now();
		if (ch.reports == 0)
			cemuLog_log(LogType::InputAPI, "WiimoteSpeaker: channel {}: stream started ({} bytes per report{}), output: {}", channel, length,
				ch.enabled ? (ch.playing ? "" : ", speaker not playing yet") : ", speaker not on", route.describe());
		else if (ch.last_route != route.describe())
			cemuLog_log(LogType::InputAPI, "WiimoteSpeaker: channel {}: output is now {}", channel, route.describe());
		ch.last_route = route.describe();
		ch.reports++;

		Result result = Result::Ok;
#ifdef SUPPORTS_WIIMOTE
		if (route.to_real)
		{
			EnsureRealSetup(ch, route, channel);
			if (!route.real->speaker_send(data, length))
			{
				ch.real_dropped++;
				result = Result::Busy;
			}
		}
#endif
		if (route.to_system && ch.enabled && !ch.muted)
		{
			int16_t samples[WiimoteSpeakerCodec::kReportDataSize * 2];
			const size_t count = ch.config.format == WiimoteSpeakerCodec::kFormatPcm8 ?
				WiimoteSpeakerCodec::DecodePcm8(data, length, samples) :
				WiimoteSpeakerCodec::DecodeAdpcm(ch.decoder, data, length, samples);
			WiimoteSpeakerMixer::Push(channel, samples, count, ch.config.SampleRate(), ch.config.Gain());
		}

		// game-side rate, every kStatsReports
		if (ch.window_start == std::chrono::steady_clock::time_point{})
			ch.window_start = now;
		if (++ch.window_reports >= kStatsReports)
		{
			const double seconds = std::chrono::duration<double>(now - ch.window_start).count();
			cemuLog_log(LogType::InputAPI, "WiimoteSpeaker: channel {}: the game sent {} reports in {:.2f} s = {:.1f}/s (speaker rate {:.1f}/s)",
				channel, ch.window_reports, seconds, ch.window_reports / seconds, 1e9 / ch.config.ReportInterval().count());
			LogStreamStats(channel, ch, "so far");
			ch.window_reports = 0;
			ch.window_start = now;
		}
		return result;
	}

	void Reset()
	{
		std::scoped_lock lock(s_mutex);
		for (auto& ch : s_channels)
			ch = Channel{};
		WiimoteSpeakerMixer::ClearAll();
	}
}
