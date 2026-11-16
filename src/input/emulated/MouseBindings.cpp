#include "input/emulated/MouseBindings.h"

#include "input/api/Mouse/MouseInputs.h"

namespace MouseBindings
{
	using namespace MouseInputs;

	Pairs GamePadTouchDefaults()
	{
		return {
			{kTouchUp, Id(kGamePad, kCursorYN)},
			{kTouchDown, Id(kGamePad, kCursorYP)},
			{kTouchLeft, Id(kGamePad, kCursorXN)},
			{kTouchRight, Id(kGamePad, kCursorXP)},
			{kTouchPress, Id(kGamePad, kLeft)},
		};
	}

	Pairs GamePadTiltDefaults()
	{
		// any game window, as before (right drag in the TV or the GamePad window)
		return {
			{kTiltUp, Id(kAny, kCursorYN)},
			{kTiltDown, Id(kAny, kCursorYP)},
			{kTiltLeft, Id(kAny, kCursorXN)},
			{kTiltRight, Id(kAny, kCursorXP)},
			{kTurnLeft, Id(kAny, kWheelUp)},
			{kTurnRight, Id(kAny, kWheelDown)},
			{kTiltHold, Id(kAny, kRight)},
		};
	}

	Pairs WiimotePointerDefaults()
	{
		return {
			{kPointerUp, Id(kTV, kCursorYN)},
			{kPointerDown, Id(kTV, kCursorYP)},
			{kPointerLeft, Id(kTV, kCursorXN)},
			{kPointerRight, Id(kTV, kCursorXP)},
		};
	}

	Pairs BoardBalanceDefaults()
	{
		// up on the screen is the front of the board
		return {
			{kBalanceUp, Id(kTV, kCursorYN)},
			{kBalanceDown, Id(kTV, kCursorYP)},
			{kBalanceLeft, Id(kTV, kCursorXN)},
			{kBalanceRight, Id(kTV, kCursorXP)},
		};
	}

	Pairs MouseDefaults(std::string_view type)
	{
		Pairs result;
		if (type == kTypeGamePad)
		{
			result = GamePadTouchDefaults();
			const auto tilt = GamePadTiltDefaults();
			result.insert(result.end(), tilt.begin(), tilt.end());
		}
		else if (type == kTypeWiimote)
		{
			result = WiimotePointerDefaults();
			result.emplace_back(kWiimoteA, Id(kTV, kLeft));
			result.emplace_back(kWiimoteB, Id(kTV, kRight));
		}
		else if (type == kTypeBoard)
			result = BoardBalanceDefaults();
		return result;
	}

	std::string MouseInputText(uint64_t input)
	{
		return "`" + std::string(kMouseDevice) + ":" + Name(input) + "`";
	}

	std::map<uint64_t, std::string> ImplicitMouseBindings(std::string_view type, const std::map<uint64_t, std::string>& bindings, bool has_wii_device)
	{
		std::map<uint64_t, std::string> result;
		const auto bound = [&](const Pairs& group) {
			for (const auto& [control, input] : group)
			{
				const auto it = bindings.find(control);
				if (it != bindings.end() && !it->second.empty())
					return true;
			}
			return false;
		};
		const auto add = [&](const Pairs& group) {
			for (const auto& [control, input] : group)
				result[control] = MouseInputText(input);
		};

		if (type == kTypeGamePad)
		{
			if (!bound(GamePadTouchDefaults()))
				add(GamePadTouchDefaults());
			if (!bound(GamePadTiltDefaults()))
				add(GamePadTiltDefaults());
		}
		else if (type == kTypeBoard)
		{
			if (!has_wii_device && !bound(BoardBalanceDefaults()))
				add(BoardBalanceDefaults());
		}
		return result;
	}
}
