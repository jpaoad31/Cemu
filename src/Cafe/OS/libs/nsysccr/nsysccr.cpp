#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/nsysccr/nsysccr.h"
#include "Cafe/OS/libs/nsysccr/DrcIrLink.h"
#include "Cafe/OS/libs/nsysccr/VirtualFitMeter.h"
#include "config/ActiveSettings.h"

#include <chrono>
#include <mutex>

// The GamePad's IR engine and the virtual Fit Meter behind it.
//
// Wii Fit U's IRC library (statically linked) uses three imports: CCRCDCPerIrdaControl (connect / send /
// receive / disconnect requests to the DRC), VPADBASEGetIRCStatus (bit 1: the link is up, bit 0: a frame
// or result waits) and VPADBASEClearIRCEvent. IRCInit does nothing unless VPADBASEIsInit is nonzero.
// Without these exports Cemu's unresolved-import stub returned 0 and the game failed with "not initialised".

namespace
{
	struct FitMeterHostState
	{
		std::recursive_mutex mutex;
		std::unique_ptr<FitMeter::VirtualFitMeter> meter;
		FitMeter::DrcIrLink link;
		std::shared_ptr<FitMeter::MeterDataSource> overrideSource;
	};

	FitMeterHostState& Host()
	{
		static FitMeterHostState s;
		return s;
	}

	uint64 NowMs()
	{
		return (uint64)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	void LogLine(const std::string& s)
	{
		cemuLog_log(LogType::FitMeter, "{}", s);
	}

	// with the host mutex held
	FitMeter::VirtualFitMeter& Meter()
	{
		auto& h = Host();
		if (!h.meter)
		{
			const fs::path dir = ActiveSettings::GetUserDataPath("fitmeter");
			std::error_code ec;
			fs::create_directories(dir, ec);
			h.meter = std::make_unique<FitMeter::VirtualFitMeter>(_pathToUtf8(dir));
			h.meter->SetLogger(LogLine);
			if (h.overrideSource)
				h.meter->SetDataSource(h.overrideSource);
			cemuLog_log(LogType::FitMeter, "Fit Meter: data source: {}", h.meter->GetDataSource()->Describe());
			h.link.SetLogger(LogLine);
			h.link.SetPeer(h.meter.get());
			if (!h.meter->LastError().empty())
				cemuLog_log(LogType::Force, "Fit Meter: {}", h.meter->LastError());
		}
		return *h.meter;
	}
}

namespace nsysccr
{
	sint32 CCRCDCPerIrdaControl(uint8 dest, uint8* request, uint32 requestSize, uint8* response, uint32 responseSize)
	{
		if (!request || !response)
			return -1;
		requestSize = std::min<uint32>(requestSize, 0x1000);
		responseSize = std::min<uint32>(responseSize, 0x1000);
		std::lock_guard lock(Host().mutex);
		Meter();
		return Host().link.Control(dest, request, requestSize, response, responseSize, NowMs());
	}

	class : public COSModule
	{
	public:
		std::string_view GetName() override
		{
			return "nsysccr";
		}

		void RPLMapped() override
		{
			cafeExportRegister("nsysccr", CCRCDCPerIrdaControl, LogType::Placeholder);
		}

		void RPLUnmapped() override
		{
			std::lock_guard lock(Host().mutex);
			Host().link.Reset();
		}
	} s_nsysccrModule;

	sint32 VPADBASEIsInit()
	{
		return 1;
	}

	uint32 VPADBASEGetIRCStatus(sint32 channel)
	{
		if (channel != 0)
			return 0;
		std::lock_guard lock(Host().mutex);
		Meter();
		return Host().link.GetStatus(NowMs());
	}

	void VPADBASEClearIRCEvent(sint32 channel)
	{
		if (channel != 0)
			return;
		std::lock_guard lock(Host().mutex);
		Host().link.ClearEvent();
	}

	class : public COSModule
	{
	public:
		std::string_view GetName() override
		{
			return "vpadbase";
		}

		void RPLMapped() override
		{
			cafeExportRegister("vpadbase", VPADBASEIsInit, LogType::Placeholder);
			cafeExportRegister("vpadbase", VPADBASEGetIRCStatus, LogType::Placeholder);
			cafeExportRegister("vpadbase", VPADBASEClearIRCEvent, LogType::Placeholder);
		}
	} s_vpadbaseModule;

	COSModule* GetModule()
	{
		return &s_nsysccrModule;
	}

	COSModule* GetVpadBaseModule()
	{
		return &s_vpadbaseModule;
	}
}

namespace FitMeterHost
{
	Status GetStatus()
	{
		auto& h = Host();
		std::lock_guard lock(h.mutex);
		auto& m = Meter();
		const auto& st = m.State();
		Status s;
		s.enabled = st.enabled;
		s.autoPress = st.autoPress;
		s.registered = st.registered;
		s.pet = st.IsPet();
		s.clockSet = st.clockSet;
		s.miiName = st.miiName;
		s.samuId = st.samuId;
		s.saveDataId = st.saveDataId;
		s.registeredAt = st.registeredAt;
		if (st.registered && st.historyStartMinute != INT64_MIN)
		{
			const sint64 hm = st.historyStartMinute % FitMeter::MINUTES_PER_DAY;
			s.historyStart = fmt::format("{} {:02}:{:02}", FitMeter::FormatDate((sint32)(st.historyStartMinute / FitMeter::MINUTES_PER_DAY)), hm / 60, hm % 60);
			const sint64 ds = m.DataStartMinute();
			s.dataStart = fmt::format("{} {:02}:{:02}", FitMeter::FormatDate((sint32)(ds / FitMeter::MINUTES_PER_DAY)), (ds % FitMeter::MINUTES_PER_DAY) / 60, ds % 60);
		}
		s.pretendDaysAgo = st.pretendDaysAgo;
		s.lastSync = st.lastSync;
		s.syncCount = st.syncCount;
		s.clock = m.Now();
		switch (h.link.GetState())
		{
		case FitMeter::DrcIrLink::State::Connected: s.link = "connected"; break;
		case FitMeter::DrcIrLink::State::Waiting: s.link = "the game is waiting for the meter"; break;
		default: s.link = m.IsConnectRequested(NowMs()) ? "the game is waiting for the meter" : "idle"; break;
		}
		s.framesIn = h.link.FramesIn();
		s.framesOut = h.link.FramesOut();
		s.folder = m.GetDir();
		s.activityPath = m.ActivityPath();
		s.lastError = m.LastError();
		s.useFile = st.source == "file";
		s.override = st.stepsOverrideDay >= 0 && st.stepsOverrideDay == m.LogicalToday();
		s.overrideSteps = st.stepsOverride;
		s.source = h.overrideSource ? h.overrideSource->Describe() : (s.useFile ? "File: " + s.activityPath : "Built-in demo (4-week cycle)");
		if (s.override)
			s.source += fmt::format(" + today's steps set to {}", st.stepsOverride);
		FitMeter::Activity act;
		std::string err;
		if (m.GetActivity(act, err))
		{
			int minuteDays = 0;
			for (const auto& [dn, d] : act.days)
				minuteDays += d.HasMinutes() ? 1 : 0;
			s.activitySummary = fmt::format("{} day(s), {} with per-minute data", act.days.size(), minuteDays);
			auto it = act.days.find(m.LogicalToday());
			if (it != act.days.end())
				s.stepsToday = it->second.steps;
			if (!h.overrideSource && !s.useFile)
				s.hint = "Sends made-up but realistic days. Load an activity.json for your own data.";
		}
		else
		{
			s.activitySummary = err;
			s.hint = "No activity.json: the meter sends no steps. Load one, or switch to the built-in demo.";
		}
		return s;
	}

	void SetEnabled(bool enabled)
	{
		std::lock_guard lock(Host().mutex);
		Meter().State().enabled = enabled;
		Meter().SaveState();
	}

	void SetAutoPress(bool autoPress)
	{
		std::lock_guard lock(Host().mutex);
		Meter().State().autoPress = autoPress;
		Meter().SaveState();
	}

	void PressButton()
	{
		std::lock_guard lock(Host().mutex);
		Meter().PressButton(NowMs());
		LogLine("Fit Meter: button pressed (UI)");
	}

	void SetPretendDaysAgo(int days)
	{
		std::lock_guard lock(Host().mutex);
		auto& st = Meter().State();
		const int d = std::clamp(days, 0, 60);
		if (d != st.pretendDaysAgo)
			st.reportClockUnsetOnce = true; // the next sync gets the full 7 days of minutes
		st.pretendDaysAgo = d;
		Meter().SaveState();
		LogLine(fmt::format("Fit Meter: test option: data starts {} day(s) before the registration", Meter().State().pretendDaysAgo));
	}

	void ClearHistory()
	{
		std::lock_guard lock(Host().mutex);
		Meter().ClearHistory();
		LogLine("Fit Meter: history cleared: no data before now (UI)");
	}

	void Unregister()
	{
		std::lock_guard lock(Host().mutex);
		Meter().Unregister();
		LogLine("Fit Meter: reset to an unregistered meter (UI)");
	}

	bool ImportActivityFile(const fs::path& source, std::string& error)
	{
		std::lock_guard lock(Host().mutex);
		FitMeter::Activity act;
		if (!FitMeter::LoadActivityFile(_pathToUtf8(source), act, error))
			return false;
		auto& m = Meter();
		m.State().activityFile = "activity.json";
		m.State().source = "file";
		m.SaveState();
		const fs::path dest = fs::path(_utf8ToPath(m.ActivityPath()));
		std::error_code ec;
		if (fs::exists(dest, ec) && !fs::equivalent(source, dest, ec))
			fs::copy_file(dest, dest.parent_path() / "activity.json.bak", fs::copy_options::overwrite_existing, ec);
		if (!fs::equivalent(source, dest, ec))
		{
			fs::copy_file(source, dest, fs::copy_options::overwrite_existing, ec);
			if (ec)
			{
				error = "can't copy the file: " + ec.message();
				return false;
			}
		}
		if (!Host().overrideSource)
			m.UseSourceFromState();
		return true;
	}

	bool SetStepsToday(sint32 steps, std::string& error)
	{
		std::lock_guard lock(Host().mutex);
		auto& m = Meter();
		m.State().stepsOverrideDay = m.LogicalToday();
		m.State().stepsOverride = std::clamp(steps, 0, 65529);
		return m.SaveState();
	}

	void ClearStepsToday()
	{
		std::lock_guard lock(Host().mutex);
		auto& m = Meter();
		m.State().stepsOverrideDay = -1;
		m.SaveState();
	}

	void UseDemo(bool demo)
	{
		std::lock_guard lock(Host().mutex);
		auto& m = Meter();
		m.State().source = demo ? "demo" : "file";
		m.SaveState();
		if (!Host().overrideSource)
			m.UseSourceFromState();
	}

	fs::path GetFolder()
	{
		std::lock_guard lock(Host().mutex);
		return _utf8ToPath(Meter().GetDir());
	}

	void SetDataSource(std::shared_ptr<FitMeter::MeterDataSource> source)
	{
		std::lock_guard lock(Host().mutex);
		auto& m = Meter();
		Host().overrideSource = source;
		if (source)
			m.SetDataSource(source);
		else
			m.UseSourceFromState();
	}
}
