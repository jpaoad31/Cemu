#include "input/InputDiagnostics.h"

#include "input/InputManager.h"
#include "input/api/Controller.h"
#include "input/emulated/BalanceBoardController.h"
#include "input/api/Mouse/MouseInputs.h"
#ifdef SUPPORTS_WIIMOTE
#include "input/api/Wiimote/NativeWiimoteController.h"
#endif

namespace InputDiagnostics
{
	namespace
	{
		std::string controller_label(const EmulatedController& c)
		{
			return fmt::format("Controller {} ({})", c.player_index() + 1, c.type_string());
		}

		std::string device_label(const ControllerBase& c)
		{
			return fmt::format("{} [{}]", c.display_name(), c.api_name());
		}

		// a device that is the whole controller: one player only
		bool is_exclusive(const ControllerBase& c)
		{
			return c.api() == InputAPI::Wiimote || c.api() == InputAPI::RemoteGamePad;
		}
	}

	bool IsDeviceConnected(ControllerBase& controller)
	{
		return controller.is_connected();
	}

	std::string DeviceStatus(ControllerBase& controller)
	{
		const bool connected = controller.is_connected();
		switch (controller.api())
		{
		case InputAPI::Keyboard:
			return "always available";
		case InputAPI::Mouse:
			return "always available (the game windows)";
		case InputAPI::RemoteGamePad:
			return connected ? "phone connected" : "waiting for the phone";
#ifdef SUPPORTS_WIIMOTE
		case InputAPI::Wiimote:
			if (const auto* wiimote = dynamic_cast<NativeWiimoteController*>(&controller); wiimote && connected)
				return wiimote->is_balance_board() ? "connected (Balance Board)" : "connected (Wii Remote)";
			return "not connected (turn it on, or connect it through the bridge)";
#endif
		default:
			return connected ? "connected" : "not found";
		}
	}

	std::vector<Warning> GetWarnings(size_t player_index)
	{
		std::vector<Warning> result;
		auto& manager = InputManager::instance();
		const auto self = manager.get_controller(player_index);
		if (!self)
			return result;

		for (const auto& device : self->get_controllers())
		{
#ifdef SUPPORTS_WIIMOTE
			// a real Wii device in the wrong role
			if (device->api() == InputAPI::Wiimote && device->is_connected())
			{
				if (const auto* wiimote = dynamic_cast<NativeWiimoteController*>(device.get()))
				{
					if (wiimote->is_balance_board() && self->type() != EmulatedController::Type::BalanceBoard)
						result.push_back({fmt::format("{} is a Balance Board: it belongs on a Balance Board controller, not this {}.", device_label(*device), self->type_string()), true});
				}
			}
#endif
			// the same device on other emulated controllers
			for (size_t i = 0; i < InputManager::kMaxController; ++i)
			{
				if (i == player_index)
					continue;
				const auto other = manager.get_controller(i);
				if (!other)
					continue;
				const auto& others = other->get_controllers();
				const auto it = std::find_if(others.cbegin(), others.cend(), [&](const auto& c) { return *c == *device; });
				if (it == others.cend())
					continue;

				if (is_exclusive(*device))
				{
					result.push_back({fmt::format("{} is also assigned to {}: one real controller can only be one emulated controller.", device_label(*device), controller_label(*other)), true});
					continue;
				}

				// shared keyboards and pads are fine as long as no input drives both players
				const auto mine = self->get_device_inputs(*device);
				const auto theirs = other->get_device_inputs(**it);
				std::vector<std::string> shared;
				for (const auto& [m, input] : mine)
				{
					if (std::any_of(theirs.cbegin(), theirs.cend(), [input = input](const auto& t) { return t.second == input; }))
					{
						auto name = device->get_button_name(input);
						if (std::find(shared.cbegin(), shared.cend(), name) == shared.cend())
							shared.emplace_back(std::move(name));
					}
				}
				if (!shared.empty())
				{
					if (shared.size() > 4)
					{
						shared.resize(4);
						shared.emplace_back("...");
					}
					result.push_back({fmt::format("{} inputs also used by {}: {}", device_label(*device), controller_label(*other), fmt::join(shared, ", ")), false});
				}
			}
		}

		// the mouse cursor driving two emulated controllers (the GamePad tilt and the board's balance
		// both following the mouse was the old implicit behaviour, now visible)
		{
			const auto uses_cursor = [](const EmulatedController& c) {
				for (const auto& device : c.get_controllers())
				{
					if (device->api() != InputAPI::Mouse)
						continue;
					for (const auto& [mapping, input] : c.get_device_inputs(*device))
					{
						if (!MouseInputs::IsPressable(input))
							return true;
					}
				}
				return false;
			};
			if (uses_cursor(*self))
			{
				for (size_t i = 0; i < InputManager::kMaxController; ++i)
				{
					const auto other = i == player_index ? nullptr : manager.get_controller(i);
					if (other && uses_cursor(*other))
						result.push_back({fmt::format("The mouse cursor also drives {}: moving the mouse moves both (clear one's Mouse bindings if that isn't wanted).", controller_label(*other)), false});
				}
			}
		}

		if (const auto board = std::dynamic_pointer_cast<BalanceBoardController>(self))
		{
			const auto info = board->get_source_info();
			if (info.warning)
				result.push_back({info.text, info.source == BalanceBoardController::Source::EmulatedFallback});
		}

		// bindings that refer to devices that aren't attached, or don't parse
		size_t broken = 0;
		for (const auto mapping : self->get_mapping_ids())
			broken += self->get_mapping_warnings(mapping).empty() ? 0 : 1;
		if (broken)
			result.push_back({fmt::format("{} binding(s) use devices that aren't attached or don't parse (shown with ?).", broken), false});
		return result;
	}

	void LogWarnings()
	{
		for (size_t i = 0; i < InputManager::kMaxController; ++i)
		{
			for (const auto& w : GetWarnings(i))
				cemuLog_log(LogType::Force, "Input: controller {}: {}", i + 1, w.text);
		}
	}
}
