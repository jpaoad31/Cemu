#include "input/api/RemoteGamePad/RemoteGamePadController.h"
#include "input/api/RemoteGamePad/RemoteGamePadMedia.h"

#include <pugixml.hpp>
#include <cstdlib>

using namespace RemoteGamePad;

namespace
{
	std::string uuid_for(const RemoteGamePadProviderSettings& settings)
	{
		return fmt::format("{}", settings.port);
	}
	std::string name_for(const RemoteGamePadProviderSettings& settings)
	{
		return fmt::format("Remote GamePad (UDP {})", settings.port);
	}

	// CEMU_REMOTE_GAMEPAD_TRACE=1: log every game read that reports a latched (already released) press
	bool trace_enabled()
	{
		static const bool enabled = std::getenv("CEMU_REMOTE_GAMEPAD_TRACE") != nullptr;
		return enabled;
	}
}

RemoteGamePadController::RemoteGamePadController()
	: RemoteGamePadController(RemoteGamePadProviderSettings{})
{
}

RemoteGamePadController::RemoteGamePadController(const RemoteGamePadProviderSettings& settings)
	: base_type(uuid_for(settings), name_for(settings), settings)
{
	// the phone always has motion: on by default (a saved profile can still turn it off)
	m_settings.motion = true;
	m_provider->start();
}

void RemoteGamePadController::save(pugi::xml_node& node)
{
	base_type::save(node);
	node.append_child("port").append_child(pugi::node_pcdata).set_value(fmt::format("{}", m_provider->get_settings().port).c_str());
	// GamePad camera and mic source (global, stored with the Remote GamePad profile)
	node.append_child("camera").append_child(pugi::node_pcdata).set_value(
		RemoteGamePad::Media::GetCameraSource() == RemoteGamePad::Media::CameraSource::Mac ? "mac" : "auto");
	node.append_child("mic").append_child(pugi::node_pcdata).set_value(
		RemoteGamePad::Media::GetMicSource() == RemoteGamePad::Media::MicSource::Host ? "host" : "auto");
	const auto speaker = RemoteGamePad::Media::GetSpeakerTarget();
	node.append_child("speaker").append_child(pugi::node_pcdata).set_value(
		speaker == RemoteGamePad::Media::SpeakerTarget::Mac ? "mac" : speaker == RemoteGamePad::Media::SpeakerTarget::Both ? "both" : "phone");
	static constexpr const char* kVideoNames[] = {"gamepad", "source720", "source1080", "source1440"};
	const int video = std::clamp((int)RemoteGamePad::Media::GetVideoResolution(), 0, 3);
	node.append_child("video_size").append_child(pugi::node_pcdata).set_value(kVideoNames[video]);
}

void RemoteGamePadController::load(const pugi::xml_node& node)
{
	base_type::load(node);

	RemoteGamePadProviderSettings settings;
	if (const auto value = node.child("port"))
		settings.port = ConvertString<uint16>(value.child_value());
	if (const auto value = node.child("camera"))
		RemoteGamePad::Media::SetCameraSource(std::string_view(value.child_value()) == "mac" ? RemoteGamePad::Media::CameraSource::Mac : RemoteGamePad::Media::CameraSource::Auto);
	if (const auto value = node.child("mic"))
		RemoteGamePad::Media::SetMicSource(std::string_view(value.child_value()) == "host" ? RemoteGamePad::Media::MicSource::Host : RemoteGamePad::Media::MicSource::Auto);
	if (const auto value = node.child("speaker"))
	{
		const std::string_view v = value.child_value();
		RemoteGamePad::Media::SetSpeakerTarget(v == "mac" ? RemoteGamePad::Media::SpeakerTarget::Mac
			: v == "both" ? RemoteGamePad::Media::SpeakerTarget::Both : RemoteGamePad::Media::SpeakerTarget::Phone);
	}

	if (const auto value = node.child("video_size"))
	{
		const std::string_view v = value.child_value();
		RemoteGamePad::Media::SetVideoResolution(v == "source720" ? RemoteGamePad::Media::VideoResolution::Source720
			: v == "source1080" ? RemoteGamePad::Media::VideoResolution::Source1080
			: v == "source1440" ? RemoteGamePad::Media::VideoResolution::Source1440
			: RemoteGamePad::Media::VideoResolution::GamePad);
	}

	const auto provider = InputManager::instance().get_api_provider(api(), settings);
	update_provider(std::dynamic_pointer_cast<RemoteGamePadControllerProvider>(provider));
	m_provider->start();
}

bool RemoteGamePadController::connect()
{
	m_provider->start();
	return is_connected();
}

bool RemoteGamePadController::is_connected()
{
	return m_provider->is_connected();
}

MotionSample RemoteGamePadController::get_motion_sample()
{
	return m_provider->get_state().motion;
}

RemoteGamePadControllerProvider::State RemoteGamePadController::read_state() const
{
	std::scoped_lock lock(m_read_mutex);
	if (m_have_read_state)
		return m_read_state;
	return m_provider->get_state();
}

bool RemoteGamePadController::has_position()
{
	const auto state = read_state();
	return state.connected && (state.input.touch_down() || state.latched_touch);
}

glm::vec2 RemoteGamePadController::get_position()
{
	const auto state = read_state();
	if (state.connected && (state.input.touch_down() || state.latched_touch))
	{
		m_prev_position = m_position;
		// a tap already lifted: report where it touched down
		if (state.input.touch_down())
			m_position = {state.input.touch_x, state.input.touch_y};
		else
			m_position = {state.latched_touch_x, state.latched_touch_y};
	}
	return m_position;
}

glm::vec2 RemoteGamePadController::get_prev_position()
{
	return m_prev_position;
}

PositionVisibility RemoteGamePadController::GetPositionVisibility()
{
	return has_position() ? PositionVisibility::FULL : PositionVisibility::NONE;
}

std::string RemoteGamePadController::get_button_name(uint64 button) const
{
	switch (button)
	{
	case kButton0: return "A";
	case kButton1: return "B";
	case kButton2: return "X";
	case kButton3: return "Y";
	case kButton4: return "L";
	case kButton5: return "R";
	case kButton6: return "ZL";
	case kButton7: return "ZR";
	case kButton8: return "+";
	case kButton9: return "-";
	case kButton10: return "Home";
	case kButton11: return "Stick L";
	case kButton12: return "Stick R";
	case kButton13: return "Up";
	case kButton14: return "Down";
	case kButton15: return "Left";
	case kButton16: return "Right";
	case kButton17: return "TV";
	}
	return base_type::get_button_name(button);
}

void RemoteGamePadController::on_emulated_read_done()
{
	uint32 buttons, touch_seq;
	bool touch;
	uint32 current_buttons;
	bool current_touch;
	float tx, ty;
	{
		std::scoped_lock lock(m_read_mutex);
		buttons = m_reported_latch;
		touch = m_reported_touch;
		touch_seq = m_reported_touch_seq;
		current_buttons = m_read_state.input.buttons;
		current_touch = m_read_state.input.touch_down();
		tx = m_read_state.latched_touch_x;
		ty = m_read_state.latched_touch_y;
		m_reported_latch = 0;
		m_reported_touch = false;
	}
	if (buttons == 0 && !touch)
		return;
	m_provider->consume_latch(buttons, touch, touch_seq);
	if (trace_enabled())
	{
		const uint32 short_presses = buttons & ~current_buttons;
		const bool short_touch = touch && !current_touch;
		if (short_presses || short_touch)
			cemuLog_log(LogType::Force, "Remote GamePad trace: game read got latched press(es): buttons {:#x}{}",
				short_presses, short_touch ? fmt::format(", touch at {:.3f},{:.3f}", tx, ty) : std::string());
	}
}

ControllerState RemoteGamePadController::raw_state()
{
	ControllerState result{};
	const auto state = m_provider->get_state();
	{
		std::scoped_lock lock(m_read_mutex);
		m_read_state = state;
		m_have_read_state = true;
		if (state.connected)
		{
			m_reported_latch |= state.latched_buttons;
			if (state.latched_touch)
			{
				m_reported_touch = true;
				m_reported_touch_seq = state.touch_latch_seq;
			}
		}
	}
	if (!state.connected)
		return result;

	const auto& in = state.input;
	// latched: pressed since the last game read, maybe already released
	const uint32 buttons = in.buttons | state.latched_buttons;
	for (int i = 0; i < kButtonCount; ++i)
	{
		if (buttons & (1u << i))
			result.buttons.SetButtonState(i, true);
	}
	// protocol: +y = up, same as the VPAD default mapping below (kAxisYP = up)
	result.axis = {in.lx, in.ly};
	result.rotation = {in.rx, in.ry};
	result.trigger = {in.zl, in.zr};
	return result;
}
