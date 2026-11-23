#include "FitMeterActivity.h"

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>

namespace FitMeter
{
	// --- dates (Howard Hinnant's civil-days algorithms) ---

	int32_t DaysFromCivil(int y, int m, int d)
	{
		y -= m <= 2;
		const int era = (y >= 0 ? y : y - 399) / 400;
		const unsigned yoe = (unsigned)(y - era * 400);
		const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
		const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
		return era * 146097 + (int)doe - 719468;
	}

	void CivilFromDays(int32_t z, int& y, int& m, int& d)
	{
		z += 719468;
		const int era = (z >= 0 ? z : z - 146096) / 146097;
		const unsigned doe = (unsigned)(z - era * 146097);
		const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
		const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
		const unsigned mp = (5 * doy + 2) / 153;
		d = (int)(doy - (153 * mp + 2) / 5 + 1);
		m = (int)(mp < 10 ? mp + 3 : mp - 9);
		y = (int)yoe + era * 400 + (m <= 2);
	}

	bool ParseDate(const std::string& s, int32_t& dayNumber)
	{
		int y, m, d;
		char c1, c2;
		std::istringstream is(s);
		if (!(is >> y >> c1 >> m >> c2 >> d) || c1 != '-' || c2 != '-')
			return false;
		if (m < 1 || m > 12 || d < 1 || d > 31 || y < 2000 || y > 2099)
			return false;
		dayNumber = DaysFromCivil(y, m, d);
		int y2, m2, d2;
		CivilFromDays(dayNumber, y2, m2, d2);
		return y2 == y && m2 == m && d2 == d;
	}

	std::string FormatDate(int32_t dayNumber)
	{
		int y, m, d;
		CivilFromDays(dayNumber, y, m, d);
		char b[16];
		std::snprintf(b, sizeof(b), "%04d-%02d-%02d", y, m, d);
		return b;
	}

	int32_t EstimateKcal(int32_t steps)
	{
		// rough: ~40 kcal per 1000 steps (a walk at ~3.5 METs for a 60 kg person)
		return steps <= 0 ? 0 : (int32_t)std::lround(steps * 0.04);
	}

	// --- JSON ---

	static bool ParseClock(const char* s, int& minute)
	{
		int h, m;
		char c;
		std::istringstream is(s);
		if (!(is >> h >> c >> m) || c != ':' || h < 0 || h > 23 || m < 0 || m > 59)
			return false;
		minute = h * 60 + m;
		return true;
	}

	template<typename T>
	static void EnsureSize(std::vector<T>& v, T fill)
	{
		if (v.empty())
			v.assign(MINUTES_PER_DAY, fill);
	}

	bool ParseActivityJson(const std::string& text, Activity& out, std::string& error)
	{
		out = Activity();
		rapidjson::Document doc;
		doc.Parse(text.c_str(), text.size());
		if (doc.HasParseError())
		{
			char b[160];
			std::snprintf(b, sizeof(b), "JSON error at %zu: %s", doc.GetErrorOffset(), rapidjson::GetParseError_En(doc.GetParseError()));
			error = b;
			return false;
		}
		if (!doc.IsObject())
		{
			error = "the file isn't a JSON object";
			return false;
		}
		if (doc.HasMember("default_tag") && doc["default_tag"].IsInt())
			out.defaultTag = std::clamp(doc["default_tag"].GetInt(), 0, 249);
		if (!doc.HasMember("days"))
			return true;
		const auto& days = doc["days"];
		if (!days.IsArray())
		{
			error = "\"days\" isn't an array";
			return false;
		}
		const float nan = std::numeric_limits<float>::quiet_NaN();
		for (const auto& jd : days.GetArray())
		{
			if (!jd.IsObject() || !jd.HasMember("date") || !jd["date"].IsString())
			{
				error = "a day without a \"date\"";
				return false;
			}
			int32_t dn;
			if (!ParseDate(jd["date"].GetString(), dn))
			{
				error = std::string("bad date \"") + jd["date"].GetString() + "\" (YYYY-MM-DD)";
				return false;
			}
			DayActivity& day = out.days[dn];
			day.day = dn;
			if (jd.HasMember("steps") && jd["steps"].IsNumber())
				day.steps = std::max(0, (int32_t)std::lround(jd["steps"].GetDouble()));
			if (jd.HasMember("kcal") && jd["kcal"].IsNumber())
				day.kcal = std::max(0, (int32_t)std::lround(jd["kcal"].GetDouble()));
			auto floats = [&](const char* name, std::vector<float>& v) -> bool {
				if (!jd.HasMember(name))
					return true;
				const auto& a = jd[name];
				if (!a.IsArray() || a.Size() > (unsigned)MINUTES_PER_DAY)
				{
					error = std::string("\"") + name + "\" must be an array of up to 1440 per-minute values";
					return false;
				}
				const float none = (&v == &day.climb) ? 0.0f : nan;
				EnsureSize(v, none);
				for (unsigned i = 0; i < a.Size(); i++)
					v[i] = a[i].IsNumber() ? (float)a[i].GetDouble() : none;
				return true;
			};
			if (!floats("mets", day.mets) || !floats("altitude", day.altitude) || !floats("climb", day.climb))
				return false;
			if (jd.HasMember("tag"))
			{
				const auto& a = jd["tag"];
				if (!a.IsArray() || a.Size() > (unsigned)MINUTES_PER_DAY)
				{
					error = "\"tag\" must be an array of up to 1440 per-minute values";
					return false;
				}
				EnsureSize<int16_t>(day.tag, -1);
				for (unsigned i = 0; i < a.Size(); i++)
					day.tag[i] = a[i].IsInt() ? (int16_t)std::clamp(a[i].GetInt(), 0, 249) : (int16_t)-1;
			}
			if (jd.HasMember("segments"))
			{
				const auto& segs = jd["segments"];
				if (!segs.IsArray())
				{
					error = "\"segments\" isn't an array";
					return false;
				}
				for (const auto& s : segs.GetArray())
				{
					int start;
					if (!s.IsObject() || !s.HasMember("start") || !s["start"].IsString() || !ParseClock(s["start"].GetString(), start))
					{
						error = "a segment needs \"start\": \"HH:MM\"";
						return false;
					}
					int minutes = (s.HasMember("minutes") && s["minutes"].IsInt()) ? s["minutes"].GetInt() : 1;
					minutes = std::clamp(minutes, 1, MINUTES_PER_DAY - start);
					if (s.HasMember("mets") && s["mets"].IsNumber())
					{
						EnsureSize(day.mets, nan);
						for (int i = 0; i < minutes; i++)
							day.mets[start + i] = (float)s["mets"].GetDouble();
					}
					if (s.HasMember("tag") && s["tag"].IsInt())
					{
						EnsureSize<int16_t>(day.tag, -1);
						for (int i = 0; i < minutes; i++)
							day.tag[start + i] = (int16_t)std::clamp(s["tag"].GetInt(), 0, 249);
					}
					if (s.HasMember("climb_m") && s["climb_m"].IsNumber())
					{
						EnsureSize(day.climb, 0.0f);
						const float per = (float)s["climb_m"].GetDouble() / (float)minutes;
						for (int i = 0; i < minutes; i++)
							day.climb[start + i] += per;
					}
				}
			}
		}
		return true;
	}

	static bool ReadFile(const std::string& path, std::string& text)
	{
		std::ifstream f(path, std::ios::binary);
		if (!f)
			return false;
		std::ostringstream ss;
		ss << f.rdbuf();
		text = ss.str();
		return true;
	}

	bool LoadActivityFile(const std::string& path, Activity& out, std::string& error)
	{
		std::string text;
		if (!ReadFile(path, text))
		{
			out = Activity();
			error = "no activity file (" + path + ")";
			return false;
		}
		if (!ParseActivityJson(text, out, error))
		{
			error = path + ": " + error;
			return false;
		}
		return true;
	}

	bool SetDayStepsInFile(const std::string& path, int32_t day, int32_t steps, std::string& error)
	{
		rapidjson::Document doc;
		std::string text;
		if (ReadFile(path, text))
		{
			doc.Parse(text.c_str(), text.size());
			if (doc.HasParseError() || !doc.IsObject())
			{
				error = path + " isn't valid JSON: not changed";
				return false;
			}
		}
		else
			doc.SetObject();
		auto& al = doc.GetAllocator();
		if (!doc.HasMember("days") || !doc["days"].IsArray())
		{
			doc.RemoveMember("days");
			doc.AddMember("days", rapidjson::Value(rapidjson::kArrayType), al);
		}
		auto& days = doc["days"];
		const std::string date = FormatDate(day);
		rapidjson::Value* entry = nullptr;
		for (auto& d : days.GetArray())
			if (d.IsObject() && d.HasMember("date") && d["date"].IsString() && date == d["date"].GetString())
				entry = &d;
		if (!entry)
		{
			rapidjson::Value d(rapidjson::kObjectType);
			d.AddMember("date", rapidjson::Value(date.c_str(), al), al);
			days.PushBack(d, al);
			entry = &days[days.Size() - 1];
		}
		if (entry->HasMember("steps"))
			(*entry)["steps"].SetInt(steps);
		else
			entry->AddMember("steps", steps, al);
		rapidjson::StringBuffer sb;
		rapidjson::PrettyWriter<rapidjson::StringBuffer> w(sb);
		w.SetIndent(' ', 2);
		doc.Accept(w);
		std::ofstream f(path, std::ios::binary | std::ios::trunc);
		if (!f)
		{
			error = "can't write " + path;
			return false;
		}
		f << sb.GetString() << "\n";
		return true;
	}

	// --- encoder ---

	void LimitToMeterMemory(Activity& a, int64_t since, int dayStartHour)
	{
		if (since == INT64_MIN)
			return;
		const int32_t sinceDay = (int32_t)(since / MINUTES_PER_DAY);
		const int sinceMin = (int)(since % MINUTES_PER_DAY);
		// the per-day totals count from the day start: the registration's logical day is the first one
		const int32_t firstLogical = sinceMin < dayStartHour * 60 ? sinceDay - 1 : sinceDay;
		for (auto it = a.days.begin(); it != a.days.end();)
		{
			DayActivity& d = it->second;
			if (it->first < firstLogical)
			{
				d.steps = d.kcal = -1;
			}
			if (it->first <= sinceDay)
			{
				const int from = it->first < sinceDay ? MINUTES_PER_DAY : sinceMin; // minutes before since: none
				for (int m = 0; m < from; m++)
				{
					if (!d.mets.empty()) d.mets[m] = NAN;
					if (!d.altitude.empty()) d.altitude[m] = NAN;
					if (!d.tag.empty()) d.tag[m] = -1;
					if (!d.climb.empty()) d.climb[m] = 0.0f;
				}
			}
			if (it->first < firstLogical && it->first < sinceDay)
				it = a.days.erase(it);
			else
				++it;
		}
	}

	std::vector<uint8_t> EncodeDayStream(const Activity& a, const MeterTime& now, int dayStartHour, bool isKcal, size_t size)
	{
		std::vector<uint8_t> out(size, 0xFF);
		const int32_t today = now.DayNumber() - (now.hour < dayStartHour ? 1 : 0);
		int32_t oldest = INT32_MAX;
		for (const auto& [dn, d] : a.days)
			if (dn <= today && (d.steps >= 0 || (isKcal && d.kcal >= 0)))
				oldest = std::min(oldest, dn);
		if (oldest == INT32_MAX)
			return out;
		const size_t entries = size / 2;
		for (size_t i = 0; i < entries; i++)
		{
			const int32_t dn = today - (int32_t)i;
			if (dn < oldest)
				break;
			int32_t v = 0; // a day inside the range without data: the meter was worn and counted nothing
			auto it = a.days.find(dn);
			if (it != a.days.end())
			{
				const DayActivity& d = it->second;
				if (isKcal)
					v = d.kcal >= 0 ? d.kcal : (d.steps >= 0 ? EstimateKcal(d.steps) : 0);
				else
					v = std::max(0, d.steps);
			}
			v = std::clamp(v, 0, 0xFFF9); // FFFC..FFFF are markers; the game caps steps at 65529
			out[i * 2] = (uint8_t)(v >> 8);
			out[i * 2 + 1] = (uint8_t)v;
		}
		return out;
	}

	std::vector<uint8_t> EncodeMinuteStream(const Activity& a, const MeterTime& now, uint8_t bank, size_t size)
	{
		std::vector<uint8_t> out(size, 0xFF);
		const int64_t nowAbs = (int64_t)now.DayNumber() * MINUTES_PER_DAY + now.hour * 60 + now.minute;
		// all three streams cover the same minutes, from the first minute with any per-minute value: the game
		// stores altitude and tag minutes only in days the METs stream created (SaveHeight / SaveTag)
		auto relevant = [&](const DayActivity& d, int m) -> bool {
			return (!d.mets.empty() && !std::isnan(d.mets[m])) || (!d.altitude.empty() && !std::isnan(d.altitude[m])) ||
				(!d.climb.empty() && d.climb[m] != 0.0f) || (!d.tag.empty() && d.tag[m] >= 0);
		};
		int64_t start = INT64_MAX;
		for (const auto& [dn, d] : a.days)
		{
			if (!d.HasMinutes())
				continue;
			for (int m = 0; m < MINUTES_PER_DAY; m++)
				if (relevant(d, m))
				{
					start = std::min<int64_t>(start, (int64_t)dn * MINUTES_PER_DAY + m);
					break;
				}
		}
		if (start == INT64_MAX || start > nowAbs)
			return out;
		const int64_t maxSpan = (int64_t)METER_MINUTE_DAYS * MINUTES_PER_DAY; // a meter's minute memory
		start = std::max(start, nowAbs - maxSpan + 1);
		const size_t n = (size_t)(nowAbs - start + 1);
		// per-minute values in time order (oldest first)
		std::vector<int> vals(n, 0);
		double level = 0.0;   // altitude as encoded so far, relative to the stream's first minute
		double ref = std::numeric_limits<double>::quiet_NaN(); // the absolute altitude at level 0
		double pending = 0.0; // metres still to send (changes over 127 / -122 per minute spill over)
		for (size_t i = 0; i < n; i++)
		{
			const int64_t abs = start + (int64_t)i;
			const int32_t dn = (int32_t)(abs / MINUTES_PER_DAY);
			const int m = (int)(abs % MINUTES_PER_DAY);
			auto it = a.days.find(dn);
			const DayActivity* d = it != a.days.end() ? &it->second : nullptr;
			if (bank == 2)
			{
				const float v = (d && !d->mets.empty()) ? d->mets[m] : NAN;
				vals[i] = std::isnan(v) ? 0 : std::clamp((int)std::lround(v * 10.0f), 0, 249);
			}
			else if (bank == 4)
			{
				int t = (d && !d->tag.empty()) ? d->tag[m] : -1;
				if (t < 0)
					t = (d && !d->mets.empty() && !std::isnan(d->mets[m])) ? a.defaultTag : 0;
				vals[i] = std::clamp(t, 0, 249);
			}
			else
			{
				// altitude: metres changed during this minute
				const float alt = (d && !d->altitude.empty()) ? d->altitude[m] : NAN;
				if (!std::isnan(alt))
				{
					if (std::isnan(ref))
						ref = std::round(alt) - level;
					pending = (std::round(alt) - ref) - level;
				}
				if (d && !d->climb.empty())
					pending += d->climb[m];
				const int delta = std::clamp((int)std::lround(pending), -122, 127);
				level += delta;
				pending -= delta;
				vals[i] = delta;
			}
		}
		// newest first, zero runs as "n 00", tokens never split by the size limit
		size_t pos = 0;
		auto put = [&](uint8_t b0, int count) -> bool {
			if (pos + count > size)
				return false;
			out[pos++] = b0;
			if (count == 2)
				out[pos++] = 0x00;
			return true;
		};
		size_t i = n;
		while (i > 0)
		{
			const int v = vals[i - 1];
			if (v == 0)
			{
				int run = 0;
				while (i > 0 && vals[i - 1] == 0 && run < 249)
				{
					run++;
					i--;
				}
				if (!put((uint8_t)run, 2))
					break;
				continue;
			}
			uint8_t b;
			if (bank == 3)
				b = v > 0 ? (uint8_t)v : (uint8_t)(0x80 + (-v - 1)); // +1..+127 = 01..7F, -1..-122 = 80..F9
			else
				b = (uint8_t)v; // 1..249
			if (!put(b, 1))
				break;
			i--;
		}
		return out;
	}
}
