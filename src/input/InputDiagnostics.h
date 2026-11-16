#pragma once

// Device roles and conflicts (fork addition, see INPUT_REWORK.md): what the input settings show
// next to each emulated controller, and what is logged when a game starts.

#include <memory>
#include <string>
#include <vector>

class ControllerBase;

namespace InputDiagnostics
{
	// "connected", "not found", "waiting for the phone", ...
	std::string DeviceStatus(ControllerBase& controller);
	bool IsDeviceConnected(ControllerBase& controller);

	struct Warning
	{
		std::string text;
		bool error = false; // a setup that can't work (vs. one that may be unintended)
	};
	// warnings for the emulated controller of a player: devices shared with other emulated
	// controllers in a conflicting way, a real Wii device in the wrong role, the board's source
	std::vector<Warning> GetWarnings(size_t player_index);
	// all players' warnings, once each, logged (when a game starts)
	void LogWarnings();
}
