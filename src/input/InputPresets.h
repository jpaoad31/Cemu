#pragma once

// Standard binding presets ("Load preset..." in the input settings; fork addition, see
// INPUT_REWORK.md). Compiled in. A preset replaces the emulated controller's devices and bindings
// with a usual setup; everything it sets shows in the binding fields and can be edited after.

#include "input/emulated/EmulatedController.h"

#include <string>
#include <vector>

namespace InputPresets
{
	struct Preset
	{
		std::string id;
		std::string name;
		std::string description;
	};
	std::vector<Preset> ForType(EmulatedController::Type type);

	struct Result
	{
		bool ok = false;
		std::string message;
	};
	Result Apply(EmulatedController& controller, const std::string& id);

	// the bindings of a preset as (control, expression) pairs, without devices (tests, docs)
	std::vector<std::pair<uint64, std::string>> KeyboardTable(EmulatedController::Type type);
}
