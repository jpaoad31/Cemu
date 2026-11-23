#pragma once
// What the virtual meter "measured": per-day steps/kcal and optional per-minute METs/altitude/activity tag,
// the data source interface (a JSON file now; the iPhone Remote GamePad app later), and the encoder that
// turns it into the meter's log streams (SAMU banks 02..06) the way Wii Fit U's decoder
// (SAMUProcess::SaveU8Data/SaveU16Data) reads them.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace FitMeter
{
	// days since 1970-01-01 (proleptic Gregorian)
	int32_t DaysFromCivil(int y, int m, int d);
	void CivilFromDays(int32_t days, int& y, int& m, int& d);
	bool ParseDate(const std::string& s, int32_t& dayNumber); // "YYYY-MM-DD"
	std::string FormatDate(int32_t dayNumber);

	constexpr int MINUTES_PER_DAY = 1440;

	struct DayActivity
	{
		int32_t day = 0;     // DaysFromCivil
		int32_t steps = -1;  // -1: not given
		int32_t kcal = -1;   // -1: not given (estimated from the steps)
		// per minute of the day (0 = 00:00), empty or 1440 entries; NaN / -1 = no value
		std::vector<float> mets;      // METs (1.0 = rest); sent as METs x 10
		std::vector<float> altitude;  // metres (absolute; the meter sends the changes)
		std::vector<int16_t> tag;     // the meter's activity tag 0..29 (meaning not known yet)
		std::vector<float> climb;     // metres gained (or lost) during the minute, from "segments" climb_m
		bool HasMinutes() const { return !mets.empty() || !altitude.empty() || !tag.empty() || !climb.empty(); }
	};

	struct Activity
	{
		std::map<int32_t, DayActivity> days;
		int defaultTag = 0; // tag for minutes that have METs but no tag
	};

	// the meter's clock reading
	struct MeterTime
	{
		int year = 2000, month = 1, day = 1, hour = 0, minute = 0;
		int32_t DayNumber() const { return DaysFromCivil(year, month, day); }
	};

	// the data source seam: the file, the built-in demo, the phone ("meter sync" message) later.
	// now: the meter's clock; dayStartHour: 3 with the night-owl setting (per-day totals count from it)
	class MeterDataSource
	{
	public:
		virtual ~MeterDataSource() = default;
		virtual bool GetActivity(const MeterTime& now, int dayStartHour, Activity& out, std::string& error) = 0;
		virtual std::string Describe() const = 0;
	};

	// activity.json:
	// { "days": [ { "date": "2026-11-21", "steps": 8000, "kcal": 250,
	//               "segments": [ { "start": "08:00", "minutes": 30, "mets": 3.5, "tag": 2, "climb_m": 12 } ],
	//               "mets": [ ... per minute ... ], "altitude": [ ... ], "climb": [ ... ], "tag": [ ... ] } ],
	//   "default_tag": 0 }
	bool ParseActivityJson(const std::string& text, Activity& out, std::string& error);
	bool LoadActivityFile(const std::string& path, Activity& out, std::string& error);
	// sets one day's steps in an activity file (creates the file / the day; other fields are kept)
	bool SetDayStepsInFile(const std::string& path, int32_t day, int32_t steps, std::string& error);

	class JsonFileMeterSource : public MeterDataSource
	{
	public:
		explicit JsonFileMeterSource(std::string path) : m_path(std::move(path)) {}
		bool GetActivity(const MeterTime&, int, Activity& out, std::string& error) override { return LoadActivityFile(m_path, out, error); }
		std::string Describe() const override { return m_path; }
		const std::string& GetPath() const { return m_path; }
	private:
		std::string m_path;
	};

	// banks 05 (kcal, isKcal) / 06 (steps): u16 BE per day, index 0 = the meter's today (the day starts at
	// dayStartHour: Wii Fit U's "night owl" setting moves it to 3:00), FFFF = no more days. size bytes.
	std::vector<uint8_t> EncodeDayStream(const Activity& a, const MeterTime& now, int dayStartHour, bool isKcal, size_t size);
	// banks 02 (METs) / 03 (altitude) / 04 (tag): one byte per minute, index 0 = the current minute, going back;
	// "n 00" = n minutes of 0; FF = padding. size bytes (the game asks 10080 = 7 days, or less). The three
	// streams start at the same minute: the first one with any per-minute value.
	std::vector<uint8_t> EncodeMinuteStream(const Activity& a, const MeterTime& now, uint8_t bank, size_t size);
	int32_t EstimateKcal(int32_t steps);
}
