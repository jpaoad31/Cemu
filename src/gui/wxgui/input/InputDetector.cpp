#include "wxgui/input/MouseFeed.h"
#include "wxgui/input/InputDetector.h"

#include "wxgui/input/MappingControl.h"
#include "wxgui/helpers/wxHelpers.h"
#include "input/InputManager.h"
#include "input/ControllerFactory.h"
#include "Cafe/CafeSystem.h"

InputDetector* InputDetector::s_instance = nullptr;

InputDetector::InputDetector()
{
	s_instance = this;
}

InputDetector::~InputDetector()
{
	if (s_instance == this)
		s_instance = nullptr;
}

bool InputDetector::update_device(ControllerBase& controller)
{
	// while a game runs, the game's reads update the Remote GamePad (its quick-tap latches belong
	// to the game's read); everything else is updated here too, as the old settings window did
	if (controller.api() == InputAPI::RemoteGamePad && CafeSystem::IsTitleRunning())
		return false;
	controller.update_state();
	return true;
}

std::vector<ControllerPtr> InputDetector::candidate_devices(const ControllerPtr& selected, const std::vector<ControllerPtr>& extra, const std::vector<ControllerPtr>& others, bool all_devices)
{
	std::vector<ControllerPtr> result;
	const auto add = [&](const ControllerPtr& c) {
		if (c && std::none_of(result.cbegin(), result.cend(), [&](const ControllerPtr& r) { return *r == *c; }))
			result.push_back(c);
	};
	add(selected);
	if (all_devices)
	{
		for (const auto& c : extra)
			add(c);
		for (const auto& c : others)
			add(c);
	}
	return result;
}

void InputDetector::start(const EmulatedControllerPtr& controller, uint64 mapping, bool append, MappingControl* control, Callback callback, bool all_devices)
{
	if (m_active)
		cancel();
	if (!controller)
		return;

	m_controller = controller;
	m_mapping = mapping;
	m_append = append;
	m_control = control;
	m_callback = std::move(callback);
	m_on_input = nullptr;
	m_key.reset();
	m_candidates.clear();

	std::vector<ControllerPtr> others;
	if (all_devices)
	{
		// Keyboard/Mouse and the connected SDL gamepads
		try
		{
			if (auto keyboard = ControllerFactory::CreateController(InputAPI::Keyboard, "keyboard", "Keyboard/Mouse"))
				others.push_back(keyboard);
		}
		catch (const std::exception&)
		{
		}
		auto& manager = InputManager::instance();
		if (manager.is_api_available(InputAPI::SDLController))
		{
			for (const auto& c : manager.get_api_provider(InputAPI::SDLController)->get_controllers())
			{
				c->connect();
				others.push_back(c);
			}
		}
	}
	for (const auto& c : candidate_devices(controller->get_device(), controller->get_extra_devices(), others, all_devices))
		m_candidates.push_back({c, {}});
	if (m_candidates.empty())
	{
		m_controller.reset();
		m_control = nullptr;
		auto cb = std::move(m_callback);
		m_callback = nullptr;
		if (cb)
		{
			Result result;
			result.message = _("No device selected: choose this controller's device first (or tick \"All devices\").").utf8_string();
			cb(result);
		}
		return;
	}

	// the click that started this is held now: it must be in the mouse's baseline
	UpdateSettingsMouse(true);
	for (auto& candidate : m_candidates)
	{
		update_device(*candidate.controller);
		candidate.baseline = candidate.controller->get_state().buttons.GetButtonList();
	}

	m_start = std::chrono::steady_clock::now();
	m_active = true;
	if (m_control)
		m_control->set_detecting((int)std::chrono::duration_cast<std::chrono::seconds>(kTimeout).count(), m_append);
}

void InputDetector::start_for_input(const EmulatedControllerPtr& controller, std::function<void(const ControllerPtr&, uint64)> on_input, Callback callback, bool all_devices)
{
	start(controller, 0, true, nullptr, std::move(callback), all_devices);
	m_on_input = std::move(on_input);
}

void InputDetector::cancel(const std::string& message)
{
	if (!m_active)
		return;
	m_active = false;
	if (m_control)
		m_control->set_detecting(-1, false);
	m_control = nullptr;
	m_candidates.clear();
	auto callback = std::move(m_callback);
	m_callback = nullptr;
	m_on_input = nullptr;
	m_controller.reset();
	if (callback)
	{
		Result result;
		result.message = message;
		callback(result);
	}
}

void InputDetector::forget_control(MappingControl* control)
{
	if (m_control == control)
	{
		m_control = nullptr;
		cancel();
	}
}

bool InputDetector::on_char_hook(wxKeyEvent& event)
{
	if (!m_active)
		return false;
	if (event.GetKeyCode() == WXK_ESCAPE)
	{
		cancel(_("Detection cancelled.").utf8_string());
		return true;
	}
	// swallowed: the key mustn't activate a button or close the dialog
	if (!m_key)
		m_key = fix_raw_keycode(event.GetRawKeyCode(), event.GetRawKeyFlags());
	return true;
}

void InputDetector::poll()
{
	if (!m_active)
		return;

	const auto elapsed = std::chrono::steady_clock::now() - m_start;
	if (elapsed >= kTimeout)
	{
		cancel(_("No input detected.").utf8_string());
		return;
	}
	if (m_control)
	{
		const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(kTimeout - elapsed).count();
		m_control->set_detecting((int)((left + 999) / 1000), m_append);
	}

	// a key from the char hook (its code is the keyboard device's button id)
	if (m_key)
	{
		const uint32 key = *m_key;
		m_key.reset();
		for (const auto& candidate : m_candidates)
		{
			if (candidate.controller->api() == InputAPI::Keyboard)
			{
				finish(candidate.controller, key);
				return;
			}
		}
	}

	for (auto& candidate : m_candidates)
	{
		auto& controller = *candidate.controller;
		update_device(controller);
		const auto& state = controller.get_state();
		const auto pressed = state.buttons.GetButtonList();
		// inputs released since the start count again
		std::erase_if(candidate.baseline, [&](uint32 id) { return std::find(pressed.cbegin(), pressed.cend(), id) == pressed.cend(); });
		for (const uint32 id : pressed)
		{
			if (std::find(candidate.baseline.cbegin(), candidate.baseline.cend(), id) != candidate.baseline.cend())
				continue;
			if (id >= kButtonAxisStart && controller.has_axis())
			{
				// stick and trigger directions: a clear deflection, and the stronger of the two axes
				const float value = controller.get_axis_value(id);
				if (value < 0.5f)
					continue;
				const auto perpendicular = [&](uint64 a, uint64 b) { return std::max(controller.get_axis_value(a), controller.get_axis_value(b)); };
				float other = 0;
				switch (id)
				{
				case kAxisXP: case kAxisXN: other = perpendicular(kAxisYP, kAxisYN); break;
				case kAxisYP: case kAxisYN: other = perpendicular(kAxisXP, kAxisXN); break;
				case kRotationXP: case kRotationXN: other = perpendicular(kRotationYP, kRotationYN); break;
				case kRotationYP: case kRotationYN: other = perpendicular(kRotationXP, kRotationXN); break;
				default: break;
				}
				if (other > value)
					continue;
			}
			finish(candidate.controller, id);
			return;
		}
	}
}

void InputDetector::finish(const ControllerPtr& controller, uint64 input)
{
	Result result;
	result.detected = true;
	auto emulated = m_controller;
	const auto selected = emulated ? emulated->get_device() : nullptr;
	result.other_device = !selected || !(*selected == *controller);
	if (m_on_input)
		m_on_input(controller, input);
	else if (emulated)
	{
		// the selected device's input is written plainly, another device's with its device
		if (m_append)
			emulated->add_mapping(m_mapping, controller, input);
		else
			emulated->replace_mapping(m_mapping, controller, input);
	}
	result.message = result.other_device ? fmt::format("{} [{}] (another device: written explicitly)", controller->get_button_name(input), controller->display_name())
		: controller->get_button_name(input);

	m_active = false;
	if (m_control)
		m_control->set_detecting(-1, false);
	m_control = nullptr;
	m_candidates.clear();
	m_on_input = nullptr;
	m_controller.reset();
	auto callback = std::move(m_callback);
	m_callback = nullptr;
	if (callback)
		callback(result);
}
