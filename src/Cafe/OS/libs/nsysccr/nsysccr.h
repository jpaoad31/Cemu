#pragma once
#include "Cafe/OS/RPL/COSModule.h"
#include "Cafe/OS/libs/nsysccr/FitMeterActivity.h"

// nsysccr (the GamePad's IR port: CCRCDCPerIrdaControl) and vpadbase (VPADBASEIsInit and the IR status),
// with the virtual Wii Fit Meter on the far end of the IR link. See FIT_METER.md.
namespace nsysccr
{
	COSModule* GetModule();
	COSModule* GetVpadBaseModule();
}

// the virtual meter, for the UI (thread-safe; created on first use, folder <user data>/fitmeter/)
namespace FitMeterHost
{
	struct Status
	{
		bool enabled = false;
		bool autoPress = false;
		bool registered = false;
		bool pet = false;
		bool clockSet = false;
		std::string miiName;
		uint32 samuId = 0;
		uint32 saveDataId = 0;
		std::string registeredAt;
		std::string lastSync;
		uint32 syncCount = 0;
		FitMeter::MeterTime clock;
		std::string link;          // "idle", "the game is waiting for the meter", "connected"
		uint64 framesIn = 0, framesOut = 0;
		std::string folder;
		std::string activityPath;
		std::string activitySummary; // e.g. "8 days, 3 with per-minute data" or the file's error
		std::string source;          // "Built-in demo (4-week cycle)", "File: ...", plus the override
		std::string hint;            // one line for the window (empty: nothing to say)
		bool useFile = false;
		bool override = false;       // today's steps are set by hand
		sint32 overrideSteps = 0;
		sint32 stepsToday = -1;      // so far (today only has the minutes up to the meter's clock)
		std::string lastError;
	};

	Status GetStatus();
	void SetEnabled(bool enabled);
	void SetAutoPress(bool autoPress);
	void PressButton();                  // the meter's button: links if the game is waiting (or within 10 s)
	void Unregister();                   // back to a new, unregistered meter (the game's save keeps its own record)
	bool ImportActivityFile(const fs::path& source, std::string& error); // copies it to <folder>/activity.json
	bool SetStepsToday(sint32 steps, std::string& error); // overrides today's step count (any source)
	void ClearStepsToday();
	void UseDemo(bool demo);             // the built-in demo (default) or the activity file
	fs::path GetFolder();
	// the data source seam: replaces the activity file (e.g. a phone sync later); nullptr: the file again
	void SetDataSource(std::shared_ptr<FitMeter::MeterDataSource> source);
}
