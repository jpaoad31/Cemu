#pragma once
// The built-in demo data: a fixed, made-up 4-week template of a person's days (commutes, stairs at work,
// lunch walks, runs, gym sessions, hikes with real climbing, lazy days, late nights out walking home after
// midnight). Calendar day d plays template day (d - epoch) mod 28, so consecutive dates are consecutive
// template days and a sync tomorrow shows today's data again as "yesterday". Today only has the minutes up
// to the meter's clock. Deterministic: no randomness at run time.

#include "FitMeterActivity.h"

#include <array>

namespace FitMeter
{
	constexpr int DEMO_CYCLE_DAYS = 28;

	struct DemoMinute
	{
		float mets = 0.0f;  // 0 = not moving
		int16_t tag = 0;
		uint16_t steps = 0;
		float climb = 0.0f; // metres
	};
	using DemoDay = std::array<DemoMinute, MINUTES_PER_DAY>;

	int DemoTemplateIndex(int32_t dayNumber); // 0..27; template day 0 is a Monday (the epoch 2024-01-01 is one)
	const DemoDay& DemoTemplateDay(int index);
	float DemoKcalPerMinute(float mets);      // active kcal (a ~60 kg person): (METs - 1) x 1.05

	class DemoMeterSource : public MeterDataSource
	{
	public:
		bool GetActivity(const SourceContext& ctx, Activity& out, std::string& error) override;
		std::string Describe() const override { return "Built-in demo (4-week cycle)"; }
	};

	// for the tools: an activity as activity.json text (per-minute "mets", "tag", "climb" arrays)
	std::string ActivityToJson(const Activity& a);
}
