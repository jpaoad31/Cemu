#include "input/InputPresets.h"

#include "input/InputManager.h"
#include "input/ControllerFactory.h"
#include "input/api/Controller.h"
#include "input/api/Mouse/MouseInputs.h"
#include "input/emulated/MouseBindings.h"
#include "input/emulated/VPADController.h"
#include "input/emulated/WiimoteController.h"
#include "input/emulated/BalanceBoardController.h"
#include "input/api/RemoteGamePad/RemoteGamePadProtocol.h"
#ifdef SUPPORTS_WIIMOTE
#include "input/api/Wiimote/NativeWiimoteController.h"
#endif

namespace InputPresets
{
	namespace
	{
		std::string key(std::string_view name)
		{
			return fmt::format("`Keyboard/keyboard:{}`", name);
		}

		std::string mouse(uint64 input)
		{
			return MouseBindings::MouseInputText(input);
		}

		using Table = std::vector<std::pair<uint64, std::string>>;

		Table to_table(const MouseBindings::Pairs& pairs)
		{
			Table result;
			for (const auto& [control, input] : pairs)
				result.emplace_back(control, mouse(input));
			return result;
		}

		std::shared_ptr<ControllerBase> attached(const EmulatedController& controller, InputAPI::Type api)
		{
			for (const auto& c : controller.get_controllers())
			{
				if (c->api() == api)
					return c;
			}
			return {};
		}

		// a connected device of an API: the one already attached, else the first the provider lists
		std::shared_ptr<ControllerBase> find_device(const EmulatedController& controller, InputAPI::Type api, bool want_board = false)
		{
			if (auto c = attached(controller, api))
				return c;
			auto& manager = InputManager::instance();
			if (!manager.is_api_available(api))
				return {};
			std::shared_ptr<ControllerBase> fallback;
			for (const auto& c : manager.get_api_provider(api)->get_controllers())
			{
#ifdef SUPPORTS_WIIMOTE
				if (api == InputAPI::Wiimote)
				{
					const auto wiimote = std::dynamic_pointer_cast<NativeWiimoteController>(c);
					if (wiimote && wiimote->is_balance_board() != want_board)
					{
						if (!fallback)
							fallback = c;
						continue;
					}
				}
#endif
				return c;
			}
			return api == InputAPI::Wiimote ? std::shared_ptr<ControllerBase>{} : fallback;
		}
	}

	std::vector<Preset> ForType(EmulatedController::Type type)
	{
		switch (type)
		{
		case EmulatedController::VPAD:
			return {
				{"vpad_keyboard_mouse", "Keyboard + mouse", "WASD left stick, IJKL right stick, arrows D-pad; touch with the left button on the GamePad window; tilt by dragging with the right button, turn with the wheel."},
				{"vpad_remote", "Remote GamePad (iPhone)", "The phone's buttons, sticks, touch screen and motion."},
				{"vpad_sdl", "Xbox / PlayStation / Switch Pro controller (SDL)", "The first connected controller by position, its motion when it has a gyro (else mouse tilt); touch with the mouse on the GamePad window."},
				{"vpad_keyboard_remote", "Keyboard + Remote GamePad", "The keyboard layout of \"Keyboard + mouse\" and the phone (buttons, touch, motion) together."},
			};
		case EmulatedController::Wiimote:
			return {
				{"wiimote_keyboard_mouse", "Keyboard + mouse pointer", "Pointer = the mouse over the TV window, A/B = left/right click there (or X/Z); 1, 2, -, = for + , H home, arrows D-pad; Nunchuk on WASD, C and Shift."},
				{"wiimote_real", "Real Wii Remote", "A connected Wii Remote (through the bridge or Bluetooth): its buttons and IR camera."},
			};
		case EmulatedController::BalanceBoard:
			return {
				{"board_emulated", "Emulated: keyboard + mouse", "Centre of balance = the mouse over the TV window; P power, O step off/on."},
				{"board_real", "Real board", "A connected Balance Board (through the bridge or Bluetooth)."},
			};
		case EmulatedController::Pro:
		case EmulatedController::Classic:
			return {
				{"pad_sdl", "SDL gamepad", "The first connected controller (Xbox, PlayStation, Switch Pro...) by position."},
			};
		default:
			return {};
		}
	}

	std::vector<std::pair<uint64, std::string>> KeyboardTable(EmulatedController::Type type)
	{
		switch (type)
		{
		case EmulatedController::VPAD:
		{
			using V = VPADController;
			return {
				{V::kButtonId_A, key("X")}, {V::kButtonId_B, key("Z")}, {V::kButtonId_X, key("C")}, {V::kButtonId_Y, key("V")},
				{V::kButtonId_L, key("Q")}, {V::kButtonId_R, key("E")}, {V::kButtonId_ZL, key("1")}, {V::kButtonId_ZR, key("3")},
				{V::kButtonId_Plus, key("Return")}, {V::kButtonId_Minus, key("Backspace")}, {V::kButtonId_Home, key("H")},
				{V::kButtonId_Up, key("Up")}, {V::kButtonId_Down, key("Down")}, {V::kButtonId_Left, key("Left")}, {V::kButtonId_Right, key("Right")},
				{V::kButtonId_StickL_Up, key("W")}, {V::kButtonId_StickL_Down, key("S")}, {V::kButtonId_StickL_Left, key("A")}, {V::kButtonId_StickL_Right, key("D")},
				{V::kButtonId_StickL, key("F")},
				{V::kButtonId_StickR_Up, key("I")}, {V::kButtonId_StickR_Down, key("K")}, {V::kButtonId_StickR_Left, key("J")}, {V::kButtonId_StickR_Right, key("L")},
				{V::kButtonId_StickR, key("G")},
				{V::kButtonId_Mic, key("M")}, {V::kButtonId_Screen, key("Tab")},
			};
		}
		case EmulatedController::Wiimote:
		{
			using W = WiimoteController;
			return {
				{W::kButtonId_A, key("X")}, {W::kButtonId_B, key("Z")}, {W::kButtonId_1, key("1")}, {W::kButtonId_2, key("2")},
				{W::kButtonId_Plus, key("=")}, {W::kButtonId_Minus, key("-")}, {W::kButtonId_Home, key("H")},
				{W::kButtonId_Up, key("Up")}, {W::kButtonId_Down, key("Down")}, {W::kButtonId_Left, key("Left")}, {W::kButtonId_Right, key("Right")},
				{W::kButtonId_Nunchuck_C, key("C")}, {W::kButtonId_Nunchuck_Z, key("Shift")},
				{W::kButtonId_Nunchuck_Up, key("W")}, {W::kButtonId_Nunchuck_Down, key("S")}, {W::kButtonId_Nunchuck_Left, key("A")}, {W::kButtonId_Nunchuck_Right, key("D")},
				{W::kButtonId_Pointer_Recenter, key("R")},
			};
		}
		case EmulatedController::BalanceBoard:
			return {
				{BalanceBoardController::kButtonId_Power, key("P")},
				{BalanceBoardController::kButtonId_StepOffToggle, key("O")},
			};
		default:
			return {};
		}
	}

	Result Apply(EmulatedController& controller, const std::string& id)
	{
		Result result;
		const auto type = controller.type();

		std::vector<std::shared_ptr<ControllerBase>> devices; // their default bindings
		Table extra;                                          // added to the defaults
		bool keyboard = false, motion = false;

		const auto make = [](InputAPI::Type api, std::string_view uuid, std::string_view name) {
			return ControllerFactory::CreateController(api, uuid, name);
		};
		const auto remote = [&]() -> std::shared_ptr<ControllerBase> {
			if (auto c = attached(controller, InputAPI::RemoteGamePad))
				return c;
			return make(InputAPI::RemoteGamePad, std::to_string(RemoteGamePad::kDefaultPort), "");
		};

		try
		{
			if (id == "vpad_keyboard_mouse")
			{
				keyboard = true;
				devices.push_back(make(InputAPI::Mouse, "mouse", "Mouse"));
			}
			else if (id == "vpad_remote")
			{
				devices.push_back(remote());
				motion = true;
			}
			else if (id == "vpad_keyboard_remote")
			{
				keyboard = true;
				devices.push_back(remote());
				motion = true;
			}
			else if (id == "vpad_sdl" || id == "pad_sdl")
			{
				auto pad = find_device(controller, InputAPI::SDLController);
				if (!pad)
				{
					result.message = "No SDL controller is connected: connect one and try again.";
					return result;
				}
				pad->connect();
				devices.push_back(pad);
				motion = true;
				if (id == "vpad_sdl")
				{
					devices.push_back(make(InputAPI::Mouse, "mouse", "Mouse"));
					// the mouse only touches; it tilts only when the pad has no gyro (below)
				}
			}
			else if (id == "wiimote_keyboard_mouse" || id == "board_emulated")
			{
				keyboard = true;
				devices.push_back(make(InputAPI::Mouse, "mouse", "Mouse"));
			}
			else if (id == "wiimote_real" || id == "board_real")
			{
				const bool board = id == "board_real";
				auto wii = find_device(controller, InputAPI::Wiimote, board);
				if (!wii)
				{
					result.message = board ? "No Balance Board is connected: connect it (bridge or Bluetooth) and try again."
						: "No Wii Remote is connected: connect it (bridge or Bluetooth) and try again.";
					return result;
				}
				devices.push_back(wii);
			}
			else
			{
				result.message = fmt::format("unknown preset {}", id);
				return result;
			}
			if (keyboard)
				devices.insert(devices.begin(), make(InputAPI::Keyboard, "keyboard", "Keyboard"));
		}
		catch (const std::exception& ex)
		{
			result.message = ex.what();
			return result;
		}

		controller.clear_controllers();
		controller.clear_mappings();
		for (const auto& device : devices)
		{
			device->connect();
			controller.add_controller(device);
			if (motion && device->has_motion())
				device->set_use_motion(true);
			// the keyboard's layout comes from the table (it has no defaults for most controllers)
			if (device->api() != InputAPI::Keyboard)
				controller.set_default_mapping(device);
		}

		if (keyboard)
		{
			const auto table = KeyboardTable(type);
			extra.insert(extra.end(), table.begin(), table.end());
		}
		if (id == "vpad_sdl")
		{
			// the mouse touches; it tilts only without a gyro
			for (const auto& [control, input] : MouseBindings::GamePadTiltDefaults())
				controller.delete_mapping(control);
			if (!controller.has_motion())
			{
				const auto tilt = to_table(MouseBindings::GamePadTiltDefaults());
				extra.insert(extra.end(), tilt.begin(), tilt.end());
			}
		}

		for (const auto& [control, text] : extra)
		{
			const auto existing = controller.get_mapping_expression(control);
			controller.set_mapping_expression(control, existing.empty() ? text : fmt::format("{} | {}", existing, text));
		}

		std::vector<std::string> names;
		for (const auto& d : devices)
			names.emplace_back(d->display_name());
		result.ok = true;
		result.message = fmt::format("Preset loaded: {}", fmt::join(names, " + "));
		return result;
	}
}
