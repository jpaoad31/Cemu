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
			return "always available (the mouse: over the game windows)";
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

		const auto device = self->get_device();
		if (!device)
			result.push_back({"No device selected: only bindings that name a device explicitly work.", false});
		else
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
			// the same device selected on other emulated controllers
			for (size_t i = 0; i < InputManager::kMaxController; ++i)
			{
				const auto other = i == player_index ? nullptr : manager.get_controller(i);
				const auto other_device = other ? other->get_device() : nullptr;
				if (!other_device || !(*other_device == *device))
					continue;
				if (is_exclusive(*device))
				{
					result.push_back({fmt::format("This device is also selected on {}: one real controller can only be one emulated controller.", controller_label(*other)), true});
					continue;
				}
				// a keyboard or a pad on two controllers is fine while no input drives both
				const auto mine = self->get_device_inputs(*device);
				const auto theirs = other->get_device_inputs(*other_device);
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
				if (shared.size() > 4)
				{
					shared.resize(4);
					shared.emplace_back("...");
				}
				result.push_back({shared.empty() ? fmt::format("This device is also selected on {}.", controller_label(*other))
					: fmt::format("This device is also selected on {}; inputs used by both: {}", controller_label(*other), fmt::join(shared, ", ")), false});
			}
		}

		// the mouse cursor driving two emulated controllers (the GamePad tilt and the board's balance
		// both following the mouse was the old implicit behaviour, now visible)
		{
			const auto uses_cursor = [](const EmulatedController& c) {
				for (const auto& d : c.get_controllers())
				{
					if (d->api() != InputAPI::Keyboard)
						continue;
					for (const auto& [mapping, input] : c.get_device_inputs(*d))
					{
						if (MouseInputs::IsKeyboardMouseId(input) && !MouseInputs::IsPressable(MouseInputs::MouseIdOf(input)))
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
						result.push_back({fmt::format("The mouse cursor also drives {}: moving the mouse moves both.", controller_label(*other)), false});
				}
			}
		}

		if (const auto board = std::dynamic_pointer_cast<BalanceBoardController>(self))
		{
			const auto info = board->get_source_info();
			if (info.warning)
				result.push_back({info.text, info.source == BalanceBoardController::Source::EmulatedFallback});
		}

		// bindings with inputs the device doesn't have, or that don't parse; explicit other devices
		size_t broken = 0, explicit_bindings = 0;
		for (const auto mapping : self->get_mapping_ids())
		{
			broken += self->get_mapping_warnings(mapping).empty() ? 0 : 1;
			explicit_bindings += self->is_mapping_explicit(mapping) ? 1 : 0;
		}
		if (broken)
			result.push_back({fmt::format("{} binding(s) use inputs the device doesn't have, devices that aren't available, or don't parse (shown with ?).", broken), false});
		if (explicit_bindings)
			result.push_back({fmt::format("{} binding(s) name another device explicitly (advanced, shown with [device]).", explicit_bindings), false});
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
