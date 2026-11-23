#include "FitMeterDemo.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

namespace FitMeter
{
	namespace
	{
		// activity kinds: METs, steps per minute, and the meter's tag on level ground / going up / going down.
		// Tags (~/temp/wii-fit-u/fitmeter/ACTIVITY_TAGS.md): tag = 10 * group + kind, group 0 level, 1 up, 2 down;
		// resting 0,1 (10,11,20,21), light 2,4 (12,22), walking 3,5 (13), running 6-9, ascending 14-19 (the only
		// minutes the Fit Meter Challenge credits height for), descending 23-29. A higher kind = more intense.
		struct Kind
		{
			float mets;
			uint16_t spm;
			int16_t tag, tagUp, tagDown;
		};
		constexpr Kind REST = {1.4f, 6, 1, 1, 1};          // a break (summit, bench)
		constexpr Kind LIGHT = {1.8f, 30, 2, 14, 23};      // moving about the office / home
		constexpr Kind EASY = {2.5f, 60, 4, 14, 23};       // pottering about
		constexpr Kind GYM = {5.5f, 14, 4, 4, 4};          // gym (weights, machines): light on the meter, few steps
		constexpr Kind STROLL = {3.0f, 92, 3, 15, 24};     // a slow walk (late at night, in the park)
		constexpr Kind WALK = {3.5f, 105, 3, 16, 24};      // a normal walk
		constexpr Kind BRISK = {4.3f, 118, 5, 16, 25};     // a brisk walk
		constexpr Kind STAIRS = {8.0f, 70, 5, 18, 26};     // stairs up
		constexpr Kind STAIRS_DOWN = {3.6f, 72, 5, 18, 26};
		constexpr Kind HIKE_UP = {6.8f, 82, 5, 17, 25};    // hiking uphill
		constexpr Kind HIKE_DOWN = {4.6f, 96, 5, 17, 25};  // and down
		constexpr Kind RUN = {9.8f, 165, 7, 19, 27};       // running (the level kind scales 6-9 with the METs)

		uint32_t Mix(uint32_t x)
		{
			x ^= x >> 16;
			x *= 0x7FEB352Du;
			x ^= x >> 15;
			x *= 0x846CA68Bu;
			x ^= x >> 16;
			return x;
		}
		// a fixed jitter in [-range, +range] for template day t, slot n
		int Jit(int t, int n, int range)
		{
			return (int)(Mix((uint32_t)(t * 131 + n * 7919 + 17)) % (uint32_t)(2 * range + 1)) - range;
		}

		void Add(DemoDay& d, int start, int minutes, const Kind& k, float climbTotal = 0.0f, int t = 0, int n = 0)
		{
			start = std::clamp(start, 0, MINUTES_PER_DAY - 1);
			minutes = std::clamp(minutes, 1, MINUTES_PER_DAY - start);
			for (int i = 0; i < minutes; i++)
			{
				DemoMinute& m = d[start + i];
				// a little minute-to-minute variation (pace changes) that is the same every time
				const int v = Jit(t * 3 + 1, n * 1000 + i, 6); // -6..+6 percent
				m.mets = k.mets * (1.0f + v / 100.0f);
				m.steps = (uint16_t)std::max(0, (int)std::lround(k.spm * (1.0f + v / 100.0f)));
				m.tag = climbTotal > 0.0f ? k.tagUp : climbTotal < 0.0f ? k.tagDown : k.tag;
				if (climbTotal == 0.0f && k.spm >= 150) // running on the level: 6..9 by intensity
					m.tag = (int16_t)std::clamp(6 + (int)((m.mets - 8.5f) / 0.8f), 6, 9);
				m.climb += climbTotal / (float)minutes;
			}
		}

		// small office/home bouts between from and to (minutes), every ~35-55 minutes
		void Bouts(DemoDay& d, int from, int to, int t, int n0)
		{
			int n = n0;
			for (int m = from + 10 + (Jit(t, n, 10) + 10); m < to; m += 45 + Jit(t, n, 10))
			{
				Add(d, m, 2 + (Jit(t, n + 500, 1) + 1), LIGHT, 0.0f, t, n);
				n++;
			}
		}

		DemoDay Build(int t)
		{
			DemoDay d{};
			const int weekday = t % 7; // 0 = Monday
			const int week = t / 7;
			auto at = [](int h, int m) { return h * 60 + m; };
			if (weekday < 5)
			{
				// commute in: walk to the station and up the stairs to the 4th floor (12 m)
				const int out = at(7, 32) + Jit(t, 1, 8);
				Add(d, out, 17 + Jit(t, 2, 2), WALK, 4.0f, t, 1);
				Add(d, out + 22, 2, STAIRS, 12.0f, t, 2);
				Bouts(d, at(8, 10), at(12, 0), t, 10);
				// lunch walk most days (long on Wednesdays)
				if (weekday != 1 && weekday != 3)
					Add(d, at(12, 15) + Jit(t, 3, 10), weekday == 2 ? 34 : 18 + Jit(t, 4, 4), weekday == 2 ? BRISK : WALK, 0.0f, t, 3);
				else
					Add(d, at(12, 30) + Jit(t, 5, 5), 6, LIGHT, 0.0f, t, 4);
				Bouts(d, at(13, 10), at(17, 20), t, 30);
				// down the stairs and home (the station is 4 m lower than the office street)
				const int home = at(17, 35) + Jit(t, 6, 12);
				Add(d, home, 2, STAIRS_DOWN, -12.0f, t, 5); // stairs down
				Add(d, home + 3, 17 + Jit(t, 7, 2), WALK, -4.0f, t, 6);
				Bouts(d, at(18, 30), at(22, 30), t, 50);
				switch (weekday)
				{
				case 0: // Monday and Thursday: gym
				case 3:
					Add(d, at(18, 50) + Jit(t, 8, 10), 48 + Jit(t, 9, 6), GYM, 0.0f, t, 7);
					break;
				case 1: // Tuesday: a run on weeks 1 and 3 (a hill on the way out), else an evening walk
					if (week % 2 == 0)
					{
						const int r = at(18, 40) + Jit(t, 10, 8);
						Add(d, r, 14, RUN, 25.0f, t, 8);
						Add(d, r + 14, 16 + Jit(t, 11, 3), RUN, -25.0f, t, 9);
						Add(d, r + 33, 12, Kind{RUN.mets * 1.08f, 172, 8, 19, 28}, 0.0f, t, 13); // a faster flat finish
					}
					else
						Add(d, at(19, 10), 24, WALK, 0.0f, t, 10);
					break;
				case 4: // Friday: a walk after dinner; weeks 1 and 3 continue into a night out (see Saturday)
					Add(d, at(20, 40) + Jit(t, 12, 10), 15, STROLL, 0.0f, t, 11);
					if (week % 2 == 0)
						Add(d, at(22, 15), 20, WALK, 0.0f, t, 12);
					break;
				default:
					break;
				}
			}
			else if (weekday == 5) // Saturday
			{
				if (week % 2 == 0)
				{
					// after Friday's night out: walking home between 00:00 and 02:00 (counts for Friday with night owl)
					Add(d, at(0, 25) + Jit(t, 20, 5), 38, STROLL, 0.0f, t, 20);
					Add(d, at(1, 12), 9, WALK, 3.0f, t, 21);
				}
				switch (week)
				{
				case 0: // errands
					Add(d, at(10, 40) + Jit(t, 22, 15), 55, STROLL, 0.0f, t, 22);
					Add(d, at(15, 5), 30, WALK, 0.0f, t, 23);
					Bouts(d, at(11, 40), at(21, 0), t, 60);
					break;
				case 1: // a hike: 450 m up and down
				case 3: // a shorter one: 380 m
				{
					const bool big = week == 1;
					const int s = at(9, 30) + Jit(t, 24, 10);
					Add(d, s, 15, WALK, 10.0f, t, 24);
					Add(d, s + 15, big ? 105 : 80, HIKE_UP, big ? 440.0f : 370.0f, t, 25);
					Add(d, s + (big ? 120 : 95), 22, REST, 0.0f, t, 26);
					Add(d, s + (big ? 142 : 117), big ? 95 : 75, HIKE_DOWN, big ? -440.0f : -370.0f, t, 27);
					Add(d, s + (big ? 237 : 192), 15, WALK, -10.0f, t, 28);
					Bouts(d, at(17, 0), at(22, 0), t, 70);
					break;
				}
				default: // week 3: a lazy day (after the walk home from the night out)
					Add(d, at(11, 20), 5, EASY, 0.0f, t, 29);
					Add(d, at(14, 50), 6, EASY, 0.0f, t, 30);
					Add(d, at(19, 30), 4, EASY, 0.0f, t, 31);
					break;
				}
			}
			else // Sunday
			{
				if (week == 1)
				{
					// after Saturday's late night: home between 00:40 and 01:45
					Add(d, at(0, 40) + Jit(t, 40, 5), 46, STROLL, -6.0f, t, 40);
					Add(d, at(1, 34), 8, WALK, 6.0f, t, 41);
				}
				switch (week)
				{
				case 0: // a long run over hills
				{
					const int r = at(8, 5) + Jit(t, 42, 10);
					for (int k = 0; k < 4; k++)
					{
						Add(d, r + k * 15, 8, RUN, 18.0f, t, 42 + k);
						Add(d, r + k * 15 + 8, 7, RUN, -18.0f, t, 46 + k);
					}
					Add(d, r + 60, 15, Kind{8.6f, 158, 6, 18, 26}, 0.0f, t, 59); // an easy flat stretch home
					Add(d, r + 75, 10, WALK, 0.0f, t, 50);
					Bouts(d, at(10, 0), at(21, 30), t, 80);
					break;
				}
				case 1: // a park walk
					Add(d, at(11, 30) + Jit(t, 51, 15), 45, STROLL, 0.0f, t, 51);
					Bouts(d, at(13, 0), at(21, 0), t, 90);
					break;
				case 2: // lazy
					Add(d, at(12, 10), 7, EASY, 0.0f, t, 52);
					Add(d, at(18, 40), 5, EASY, 0.0f, t, 53);
					break;
				default: // a walk to the old town: 220 steps of stairs to the castle
					Add(d, at(14, 0) + Jit(t, 54, 10), 40, WALK, 0.0f, t, 54);
					Add(d, at(14, 45), 6, STAIRS, 30.0f, t, 55);
					Add(d, at(14, 51), 25, STROLL, 0.0f, t, 56);
					Add(d, at(15, 20), 5, STAIRS_DOWN, -30.0f, t, 57);
					Add(d, at(15, 25), 38, WALK, 0.0f, t, 58);
					Bouts(d, at(16, 30), at(21, 30), t, 100);
					break;
				}
			}
			return d;
		}

		const std::array<DemoDay, DEMO_CYCLE_DAYS>& Template()
		{
			static const std::array<DemoDay, DEMO_CYCLE_DAYS> s = [] {
				std::array<DemoDay, DEMO_CYCLE_DAYS> a{};
				for (int t = 0; t < DEMO_CYCLE_DAYS; t++)
					a[t] = Build(t);
				return a;
			}();
			return s;
		}
	}

	int DemoTemplateIndex(int32_t dayNumber)
	{
		static const int32_t epoch = DaysFromCivil(2024, 1, 1); // a Monday
		return (int)(((dayNumber - epoch) % DEMO_CYCLE_DAYS + DEMO_CYCLE_DAYS) % DEMO_CYCLE_DAYS);
	}

	const DemoDay& DemoTemplateDay(int index)
	{
		return Template()[(size_t)std::clamp(index, 0, DEMO_CYCLE_DAYS - 1)];
	}

	float DemoKcalPerMinute(float mets)
	{
		return mets > 1.0f ? (mets - 1.0f) * 1.05f : 0.0f;
	}

	bool DemoMeterSource::GetActivity(const SourceContext& ctx, Activity& out, std::string&)
	{
		const MeterTime& now = ctx.now;
		const int dayStartHour = ctx.dayStartHour;
		out = Activity();
		const int32_t today = now.DayNumber();
		const int nowMinute = now.hour * 60 + now.minute;
		const float nan = std::numeric_limits<float>::quiet_NaN();
		constexpr int DAYS = 50; // what the game's save keeps
		std::map<int32_t, double> steps, kcal;
		for (int32_t c = today - DAYS; c <= today; c++)
		{
			const DemoDay& tpl = DemoTemplateDay(DemoTemplateIndex(c));
			const int last = c == today ? nowMinute : MINUTES_PER_DAY - 1;
			DayActivity& day = out.days[c];
			day.day = c;
			day.mets.assign(MINUTES_PER_DAY, nan);
			day.tag.assign(MINUTES_PER_DAY, -1);
			day.climb.assign(MINUTES_PER_DAY, 0.0f);
			for (int m = 0; m <= last; m++)
			{
				const DemoMinute& dm = tpl[m];
				if ((int64_t)c * MINUTES_PER_DAY + m < ctx.sinceMinute)
					continue; // before the meter's memory starts (its registration)
				day.mets[m] = dm.mets;
				day.tag[m] = dm.mets > 0.0f ? dm.tag : 0;
				day.climb[m] = dm.climb;
				// per-day totals count from the day start (3:00 with night owl): 00:00-02:59 is the day before
				const int32_t logical = m < dayStartHour * 60 ? c - 1 : c;
				steps[logical] += dm.steps;
				kcal[logical] += DemoKcalPerMinute(dm.mets);
			}
		}
		for (auto& [dn, day] : out.days)
		{
			if (dn == today - DAYS && dayStartHour > 0)
				continue; // only partly covered: its early hours fell before the range
			if (!steps.count(dn))
				continue; // nothing since the meter's memory starts
			day.steps = (int32_t)std::lround(steps[dn]);
			day.kcal = (int32_t)std::lround(kcal[dn]);
		}
		LimitToMeterMemory(out, ctx.sinceMinute, dayStartHour);
		// with night owl before 3:00, "today" for the totals is still yesterday; the calendar day only has minutes
		if (dayStartHour > 0 && nowMinute < dayStartHour * 60)
		{
			out.days[today].steps = -1;
			out.days[today].kcal = -1;
		}
		return true;
	}

	std::string ActivityToJson(const Activity& a)
	{
		std::string s = "{ \"default_tag\": " + std::to_string(a.defaultTag) + ",\n  \"days\": [\n";
		bool firstDay = true;
		char b[64];
		for (const auto& [dn, d] : a.days)
		{
			s += firstDay ? "    { " : ",\n    { ";
			firstDay = false;
			s += "\"date\": \"" + FormatDate(dn) + "\"";
			if (d.steps >= 0)
				s += ", \"steps\": " + std::to_string(d.steps);
			if (d.kcal >= 0)
				s += ", \"kcal\": " + std::to_string(d.kcal);
			auto arr = [&](const char* name, size_t n, auto get) {
				if (!n)
					return;
				s += std::string(",\n      \"") + name + "\": [";
				for (size_t i = 0; i < n; i++)
				{
					s += i ? "," : "";
					s += get(i);
				}
				s += "]";
			};
			arr("mets", d.mets.size(), [&](size_t i) -> std::string {
				if (std::isnan(d.mets[i]))
					return "null";
				std::snprintf(b, sizeof(b), "%.9g", d.mets[i]);
				return b;
			});
			arr("altitude", d.altitude.size(), [&](size_t i) -> std::string {
				if (std::isnan(d.altitude[i]))
					return "null";
				std::snprintf(b, sizeof(b), "%.9g", d.altitude[i]);
				return b;
			});
			arr("tag", d.tag.size(), [&](size_t i) -> std::string { return d.tag[i] < 0 ? "null" : std::to_string(d.tag[i]); });
			arr("climb", d.climb.size(), [&](size_t i) -> std::string {
				std::snprintf(b, sizeof(b), "%.9g", d.climb[i]);
				return b;
			});
			s += " }";
		}
		s += "\n  ] }\n";
		return s;
	}
}
