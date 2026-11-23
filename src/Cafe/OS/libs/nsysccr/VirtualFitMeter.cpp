#include "VirtualFitMeter.h"
#include "FitMeterProtocol.h"

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace FitMeter
{
	// --- helpers ---

	static std::string ToHex(const std::vector<uint8_t>& v)
	{
		static const char* d = "0123456789abcdef";
		std::string s;
		s.reserve(v.size() * 2);
		for (uint8_t b : v)
		{
			s += d[b >> 4];
			s += d[b & 15];
		}
		return s;
	}

	static std::vector<uint8_t> FromHex(const std::string& s)
	{
		std::vector<uint8_t> v;
		auto nib = [](char c) -> int {
			if (c >= '0' && c <= '9') return c - '0';
			if (c >= 'a' && c <= 'f') return c - 'a' + 10;
			if (c >= 'A' && c <= 'F') return c - 'A' + 10;
			return -1;
		};
		for (size_t i = 0; i + 1 < s.size(); i += 2)
		{
			const int a = nib(s[i]), b = nib(s[i + 1]);
			if (a < 0 || b < 0)
				break;
			v.push_back((uint8_t)(a * 16 + b));
		}
		return v;
	}

	static bool ReadAll(const fs::path& p, std::vector<uint8_t>& out)
	{
		std::ifstream f(p, std::ios::binary);
		if (!f)
			return false;
		out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
		return true;
	}

	static bool WriteAll(const fs::path& p, const void* data, size_t size)
	{
		std::ofstream f(p, std::ios::binary | std::ios::trunc);
		if (!f)
			return false;
		f.write((const char*)data, (std::streamsize)size);
		return (bool)f;
	}

	static std::tm LocalTm(int64_t t)
	{
		std::time_t tt = (std::time_t)t;
		std::tm tm{};
#if defined(_WIN32)
		localtime_s(&tm, &tt);
#else
		localtime_r(&tt, &tm);
#endif
		return tm;
	}

	static std::string BlobKey(uint8_t cmd, uint16_t arg)
	{
		char b[16];
		std::snprintf(b, sizeof(b), "%02x_%02x", cmd, arg);
		return b;
	}

	std::string MiiNameFromStoreData(const std::vector<uint8_t>& d)
	{
		// FFLStoreData: the Mii's name at 0x1A, 10 UTF-16 code units. Pick the byte order that reads as text.
		if (d.size() < 0x1A + 20)
			return {};
		auto decode = [&](bool bigEndian, int& score) {
			std::u16string s;
			score = 0;
			for (int i = 0; i < 10; i++)
			{
				const uint8_t a = d[0x1A + i * 2], b = d[0x1A + i * 2 + 1];
				const char16_t c = bigEndian ? (char16_t)((a << 8) | b) : (char16_t)((b << 8) | a);
				if (c == 0)
					break;
				s += c;
				score += (c >= 0x20 && c < 0x3000) ? 2 : -3;
			}
			return s;
		};
		int sb, sl;
		const auto be = decode(true, sb);
		const auto le = decode(false, sl);
		const std::u16string& u = sb >= sl ? be : le;
		std::string out; // UTF-8
		for (char16_t c : u)
		{
			if (c < 0x80)
				out += (char)c;
			else if (c < 0x800)
			{
				out += (char)(0xC0 | (c >> 6));
				out += (char)(0x80 | (c & 0x3F));
			}
			else
			{
				out += (char)(0xE0 | (c >> 12));
				out += (char)(0x80 | ((c >> 6) & 0x3F));
				out += (char)(0x80 | (c & 0x3F));
			}
		}
		return out;
	}

	// --- state ---

	int MeterState::DayStartHour() const
	{
		return (personal.size() > 3 && personal[3] != 0) ? 3 : 0;
	}

	bool MeterState::Load(const std::string& dir, std::string& error)
	{
		*this = MeterState();
		const fs::path path = fs::path(dir) / "meter.json";
		std::vector<uint8_t> raw;
		if (!ReadAll(path, raw))
			return true; // a new meter
		rapidjson::Document doc;
		doc.Parse((const char*)raw.data(), raw.size());
		if (doc.HasParseError() || !doc.IsObject())
		{
			error = path.string() + " isn't valid JSON (using a new meter)";
			return false;
		}
		auto b = [&](const char* k, bool& v) { if (doc.HasMember(k) && doc[k].IsBool()) v = doc[k].GetBool(); };
		auto u = [&](const char* k, uint32_t& v) { if (doc.HasMember(k) && doc[k].IsUint()) v = doc[k].GetUint(); };
		auto s = [&](const char* k, std::string& v) { if (doc.HasMember(k) && doc[k].IsString()) v = doc[k].GetString(); };
		auto h = [&](const char* k, std::vector<uint8_t>& v) { if (doc.HasMember(k) && doc[k].IsString()) v = FromHex(doc[k].GetString()); };
		b("enabled", enabled);
		b("auto_press", autoPress);
		if (doc.HasMember("auto_press_delay_ms") && doc["auto_press_delay_ms"].IsInt())
			autoPressDelayMs = std::clamp(doc["auto_press_delay_ms"].GetInt(), 0, 30000);
		s("activity_file", activityFile);
		s("source", source);
		if (source != "file")
			source = "demo";
		if (doc.HasMember("steps_override_day") && doc["steps_override_day"].IsString())
		{
			int32_t dn;
			if (ParseDate(doc["steps_override_day"].GetString(), dn))
				stepsOverrideDay = dn;
		}
		if (doc.HasMember("steps_override") && doc["steps_override"].IsInt())
			stepsOverride = doc["steps_override"].GetInt();
		b("registered", registered);
		u("save_data_id", saveDataId);
		u("samu_id", samuId);
		uint32_t flag = identFlag;
		u("ident_flag", flag);
		identFlag = (uint8_t)flag;
		b("clock_set", clockSet);
		if (doc.HasMember("clock_offset_s") && doc["clock_offset_s"].IsInt64())
			clockOffsetSeconds = doc["clock_offset_s"].GetInt64();
		h("mii_info", miiInfo);
		h("personal", personal);
		s("mii_name", miiName);
		s("registered_at", registeredAt);
		if (doc.HasMember("pretend_registered_days_ago") && doc["pretend_registered_days_ago"].IsInt())
			pretendDaysAgo = std::clamp(doc["pretend_registered_days_ago"].GetInt(), 0, 60);
		b("report_clock_unset_once", reportClockUnsetOnce);
		if (doc.HasMember("history_start") && doc["history_start"].IsString())
		{
			int y, mo, d, h, mi;
			if (std::sscanf(doc["history_start"].GetString(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) == 5)
				historyStartMinute = (int64_t)DaysFromCivil(y, mo, d) * MINUTES_PER_DAY + h * 60 + mi;
		}
		else if (registered && !registeredAt.empty())
		{
			// meter.json from before history_start: the registration time (host clock = meter clock + offset)
			int y, mo, d, h, mi;
			if (std::sscanf(registeredAt.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) == 5)
				historyStartMinute = (int64_t)DaysFromCivil(y, mo, d) * MINUTES_PER_DAY + h * 60 + mi + clockOffsetSeconds / 60;
		}
		s("last_sync", lastSync);
		u("sync_count", syncCount);
		if (doc.HasMember("blobs") && doc["blobs"].IsArray())
		{
			for (const auto& k : doc["blobs"].GetArray())
			{
				if (!k.IsString())
					continue;
				std::vector<uint8_t> data;
				if (ReadAll(fs::path(dir) / "blobs" / (std::string(k.GetString()) + ".bin"), data))
					blobs[k.GetString()] = std::move(data);
			}
		}
		return true;
	}

	bool MeterState::Save(const std::string& dir, std::string& error) const
	{
		std::error_code ec;
		fs::create_directories(fs::path(dir) / "blobs", ec);
		rapidjson::Document doc;
		doc.SetObject();
		auto& al = doc.GetAllocator();
		auto str = [&](const std::string& v) { return rapidjson::Value(v.c_str(), (rapidjson::SizeType)v.size(), al); };
		doc.AddMember("version", 1, al);
		doc.AddMember("enabled", enabled, al);
		doc.AddMember("auto_press", autoPress, al);
		doc.AddMember("auto_press_delay_ms", autoPressDelayMs, al);
		doc.AddMember("activity_file", str(activityFile), al);
		doc.AddMember("source", str(source), al);
		doc.AddMember("steps_override_day", str(stepsOverrideDay >= 0 ? FormatDate(stepsOverrideDay) : std::string()), al);
		doc.AddMember("steps_override", stepsOverride, al);
		doc.AddMember("registered", registered, al);
		doc.AddMember("save_data_id", saveDataId, al);
		doc.AddMember("samu_id", samuId, al);
		doc.AddMember("ident_flag", (unsigned)identFlag, al);
		doc.AddMember("clock_set", clockSet, al);
		doc.AddMember("clock_offset_s", (int64_t)clockOffsetSeconds, al);
		doc.AddMember("mii_name", str(miiName), al);
		doc.AddMember("mii_info", str(ToHex(miiInfo)), al);
		doc.AddMember("personal", str(ToHex(personal)), al);
		doc.AddMember("registered_at", str(registeredAt), al);
		{
			std::string hs;
			if (historyStartMinute != INT64_MIN)
			{
				char b[32];
				const int64_t m = historyStartMinute % MINUTES_PER_DAY;
				std::snprintf(b, sizeof(b), " %02d:%02d", (int)(m / 60), (int)(m % 60));
				hs = FormatDate((int32_t)(historyStartMinute / MINUTES_PER_DAY)) + b;
			}
			doc.AddMember("history_start", str(hs), al);
			doc.AddMember("pretend_registered_days_ago", pretendDaysAgo, al);
			doc.AddMember("report_clock_unset_once", reportClockUnsetOnce, al);
		}
		doc.AddMember("last_sync", str(lastSync), al);
		doc.AddMember("sync_count", syncCount, al);
		rapidjson::Value keys(rapidjson::kArrayType);
		for (const auto& [k, v] : blobs) // the files are written as they arrive (VirtualFitMeter::HandleWrite)
			keys.PushBack(str(k), al);
		doc.AddMember("blobs", keys, al);
		rapidjson::StringBuffer sb;
		rapidjson::PrettyWriter<rapidjson::StringBuffer> w(sb);
		w.SetIndent(' ', 2);
		doc.Accept(w);
		const fs::path tmp = fs::path(dir) / "meter.json.tmp";
		if (!WriteAll(tmp, sb.GetString(), sb.GetSize()))
		{
			error = "can't write " + tmp.string();
			return false;
		}
		fs::rename(tmp, fs::path(dir) / "meter.json", ec);
		if (ec)
		{
			error = "can't replace meter.json: " + ec.message();
			return false;
		}
		return true;
	}

	// --- the meter ---

	VirtualFitMeter::VirtualFitMeter(std::string dir) : m_dir(std::move(dir))
	{
		m_wallClock = [] {
			return (int64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
		};
		std::string err;
		if (!m_state.Load(m_dir, err))
			m_lastError = err;
		UseSourceFromState();
	}

	void VirtualFitMeter::UseSourceFromState()
	{
		if (m_state.source == "file")
			m_source = std::make_shared<JsonFileMeterSource>(ActivityPath());
		else
			m_source = std::make_shared<DemoMeterSource>();
	}

	int32_t VirtualFitMeter::LogicalToday() const
	{
		const MeterTime t = Now();
		return t.DayNumber() - (t.hour < m_state.DayStartHour() ? 1 : 0);
	}

	int64_t VirtualFitMeter::NowMinute() const
	{
		const MeterTime t = Now();
		return (int64_t)t.DayNumber() * MINUTES_PER_DAY + t.hour * 60 + t.minute;
	}

	int64_t VirtualFitMeter::DataStartMinute() const
	{
		if (!m_state.registered || m_state.historyStartMinute == INT64_MIN)
			return INT64_MIN;
		return m_state.historyStartMinute - (int64_t)m_state.pretendDaysAgo * MINUTES_PER_DAY;
	}

	void VirtualFitMeter::ClearHistory()
	{
		m_state.historyStartMinute = NowMinute();
		SaveState();
	}

	bool VirtualFitMeter::GetActivity(Activity& act, std::string& err)
	{
		bool ok = true;
		SourceContext ctx;
		ctx.now = Now();
		ctx.dayStartHour = m_state.DayStartHour();
		// a meter only has data from after its registration
		ctx.sinceMinute = DataStartMinute(); // (the test option can move it back)
		if (m_source)
			ok = m_source->GetActivity(ctx, act, err);
		LimitToMeterMemory(act, ctx.sinceMinute, ctx.dayStartHour);
		if (m_state.stepsOverrideDay >= 0 && m_state.stepsOverrideDay == LogicalToday())
		{
			DayActivity& d = act.days[m_state.stepsOverrideDay];
			d.day = m_state.stepsOverrideDay;
			if (d.steps != m_state.stepsOverride)
				d.kcal = -1; // estimated from the steps
			d.steps = m_state.stepsOverride;
		}
		return ok;
	}

	std::string VirtualFitMeter::ActivityPath() const
	{
		fs::path p(m_state.activityFile);
		if (p.is_absolute())
			return p.string();
		return (fs::path(m_dir) / p).string();
	}

	bool VirtualFitMeter::SaveState()
	{
		std::string err;
		if (!m_state.Save(m_dir, err))
		{
			m_lastError = err;
			Log("Fit Meter: " + err);
			return false;
		}
		return true;
	}

	void VirtualFitMeter::Unregister()
	{
		m_state.registered = false;
		m_state.saveDataId = 0xFFFFFFFF;
		m_state.samuId = 0xFFFFFFFF;
		m_state.identFlag = 0xFF;
		m_state.miiInfo.clear();
		m_state.miiName.clear();
		m_state.personal.clear();
		m_state.registeredAt.clear();
		m_state.historyStartMinute = INT64_MIN;
		m_state.lastSync.clear();
		m_state.syncCount = 0;
		for (auto it = m_state.blobs.begin(); it != m_state.blobs.end();)
		{
			std::error_code ec;
			fs::remove(fs::path(m_dir) / "blobs" / (it->first + ".bin"), ec);
			it = m_state.blobs.erase(it);
		}
		SaveState();
	}

	void VirtualFitMeter::PressButton(uint64_t nowMs)
	{
		m_buttonPressed = true;
		m_buttonPressedMs = nowMs;
	}

	MeterTime VirtualFitMeter::Now() const
	{
		const std::tm tm = LocalTm(m_wallClock() + m_state.clockOffsetSeconds);
		MeterTime t;
		t.year = tm.tm_year + 1900;
		t.month = tm.tm_mon + 1;
		t.day = tm.tm_mday;
		t.hour = tm.tm_hour;
		t.minute = tm.tm_min;
		return t;
	}

	std::string VirtualFitMeter::NowString() const
	{
		const std::tm tm = LocalTm(m_wallClock());
		char b[32];
		std::strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &tm);
		return b;
	}

	// the game retries a 1000 ms connect for as long as it shows "point the Fit Meter at the GamePad":
	// back-to-back attempts are one request
	static constexpr uint64_t CONNECT_RETRY_GAP_MS = 5000;

	void VirtualFitMeter::OnConnectRequested(uint64_t nowMs)
	{
		if (!m_connectRequested || nowMs - m_connectActivityMs > CONNECT_RETRY_GAP_MS)
			m_connectRequestedMs = nowMs;
		m_connectRequested = true;
		m_connectActivityMs = nowMs;
	}

	void VirtualFitMeter::OnConnectAbandoned()
	{
		// keep the request: the next attempt usually follows at once
	}

	bool VirtualFitMeter::IsConnectRequested(uint64_t nowMs) const
	{
		return m_connectRequested && nowMs - m_connectActivityMs <= CONNECT_RETRY_GAP_MS;
	}

	bool VirtualFitMeter::WantsLink(uint64_t nowMs)
	{
		if (!m_state.enabled)
			return false;
		if (m_buttonPressed)
		{
			if (nowMs - m_buttonPressedMs < 10000)
				return true;
			m_buttonPressed = false;
		}
		if (!IsConnectRequested(nowMs))
			return false;
		m_connectActivityMs = nowMs;
		return m_state.autoPress && nowMs - m_connectRequestedMs >= (uint64_t)m_state.autoPressDelayMs;
	}

	void VirtualFitMeter::OnLinkUp(uint8_t connectionType, uint64_t nowMs)
	{
		m_linked = true;
		m_connectRequested = false;
		m_buttonPressed = false;
		m_xfer = Xfer::Idle;
		m_out.clear();
		m_buffer.clear();
		// the meter started this link (the console waited for it): it says "ready"
		if (connectionType == 0)
			Send({IRCU_ACK_LAST});
	}

	void VirtualFitMeter::OnLinkDown()
	{
		m_linked = false;
		m_xfer = Xfer::Idle;
		m_out.clear();
		m_buffer.clear();
	}

	bool VirtualFitMeter::PopSend(std::vector<uint8_t>& payload)
	{
		if (m_out.empty())
			return false;
		payload = std::move(m_out.front());
		m_out.pop_front();
		return true;
	}

	void VirtualFitMeter::Send(std::vector<uint8_t> payload)
	{
		m_lastSent = payload;
		m_out.push_back(std::move(payload));
	}

	void VirtualFitMeter::SendNextChunk(uint16_t replyHint)
	{
		size_t maxData = replyHint > 1 ? (size_t)replyHint - 1 : IRCU_MAX_CHUNK;
		maxData = std::clamp<size_t>(maxData, 1, IRCU_MAX_CHUNK);
		const size_t n = std::min(maxData, m_buffer.size() - m_readPos);
		const bool last = m_readPos + n >= m_buffer.size();
		std::vector<uint8_t> p;
		p.reserve(n + 1);
		p.push_back(last ? IRCU_DATA_LAST : IRCU_DATA);
		p.insert(p.end(), m_buffer.begin() + (ptrdiff_t)m_readPos, m_buffer.begin() + (ptrdiff_t)(m_readPos + n));
		m_readPos += n;
		m_xfer = last ? Xfer::ReadWaitLastAck : Xfer::ReadData;
		Send(std::move(p));
	}

	void VirtualFitMeter::OnReceive(const std::vector<uint8_t>& p, uint16_t replyHint, uint64_t nowMs)
	{
		if (p.empty())
			return;
		switch (p[0])
		{
		case IRCU_COMMAND:
		{
			if (p.size() < 4)
			{
				Log("Fit Meter: short F4 command");
				return;
			}
			if (m_xfer == Xfer::WriteData && m_buffer.empty())
			{
				// the previous write had no data (IRCUMasterSend with 0 bytes)
				HandleWrite(m_cmd, m_arg, {});
			}
			m_cmd = p[1];
			m_arg = (uint16_t)((p[2] << 8) | p[3]);
			m_buffer.clear();
			m_readPos = 0;
			if (m_cmd & 0x80)
			{
				m_xfer = Xfer::WriteData;
				Send({IRCU_ACK});
				return;
			}
			auto data = HandleRead(m_cmd, m_arg);
			if (!data)
			{
				m_xfer = Xfer::Idle;
				return; // no answer: the console times out
			}
			XorSamu(*data);
			m_buffer = std::move(*data);
			if (m_buffer.empty())
			{
				m_xfer = Xfer::Idle;
				return;
			}
			SendNextChunk(replyHint);
			return;
		}
		case IRCU_DATA:
		case IRCU_DATA_LAST:
		{
			if (m_xfer != Xfer::WriteData)
				Log("Fit Meter: data without a write command; acked");
			else
				m_buffer.insert(m_buffer.end(), p.begin() + 1, p.end());
			if (p[0] == IRCU_DATA)
			{
				Send({IRCU_ACK});
				return;
			}
			if (m_xfer == Xfer::WriteData)
			{
				std::vector<uint8_t> data = std::move(m_buffer);
				m_buffer.clear();
				XorSamu(data);
				HandleWrite(m_cmd, m_arg, data);
			}
			m_xfer = Xfer::Idle;
			Send({IRCU_ACK_LAST});
			return;
		}
		case IRCU_ACK:
			if (m_xfer == Xfer::ReadData)
				SendNextChunk(replyHint);
			return;
		case IRCU_ACK_LAST:
			if (m_xfer == Xfer::ReadWaitLastAck)
			{
				m_xfer = Xfer::Idle;
				Send({IRCU_ACK_LAST});
			}
			return; // else: the console's "ready" (a link it started), nothing to answer
		case IRCU_RESEND:
			if (!m_lastSent.empty())
				m_out.push_back(m_lastSent);
			return;
		default:
			Log("Fit Meter: unknown IRCU packet " + HexBytes(p.data(), p.size(), 8));
			return;
		}
	}

	std::optional<std::vector<uint8_t>> VirtualFitMeter::HandleRead(uint8_t cmd, uint16_t arg)
	{
		char b[200];
		switch (cmd)
		{
		case SAMU_READ_IDENT:
		{
			std::vector<uint8_t> d(9, 0xFF);
			if (m_state.registered)
			{
				const uint32_t a = m_state.saveDataId, s = m_state.samuId;
				d = {(uint8_t)(a >> 24), (uint8_t)(a >> 16), (uint8_t)(a >> 8), (uint8_t)a,
				     (uint8_t)(s >> 24), (uint8_t)(s >> 16), (uint8_t)(s >> 8), (uint8_t)s, 0x00};
			}
			std::snprintf(b, sizeof(b), "Fit Meter: ident -> %s (saveDataId %08X, samuId %08X)",
				m_state.registered ? "registered" : "not registered", m_state.saveDataId, m_state.samuId);
			Log(b);
			return d;
		}
		case SAMU_READ_TIME:
		{
			const MeterTime t = Now();
			// after the test option changed: "clock not set" once, so the game resets its transfer date, asks for the
			// full 7 days of minutes (SAMUSeq: lbl_11C3FA24 = 10080) and writes the time
			const bool unset = !m_state.clockSet || m_state.reportClockUnsetOnce;
			std::vector<uint8_t> d = {(uint8_t)(unset ? 0xFF : 0x00), (uint8_t)(t.year - 2000), (uint8_t)t.month,
				(uint8_t)t.day, (uint8_t)t.hour, (uint8_t)t.minute};
			std::snprintf(b, sizeof(b), "Fit Meter: time -> %04d-%02d-%02d %02d:%02d%s", t.year, t.month, t.day, t.hour, t.minute,
				m_state.clockSet ? "" : " (clock not set)");
			Log(b);
			return d;
		}
		case SAMU_READ_METS:
		case SAMU_READ_ALTITUDE:
		case SAMU_READ_TAG:
		case SAMU_READ_KCAL:
		case SAMU_READ_STEPS:
		{
			Activity act;
			std::string err;
			if (!GetActivity(act, err))
			{
				m_lastError = err;
				Log("Fit Meter: " + err + " (sending no data)");
			}
			const MeterTime t = Now();
			std::vector<uint8_t> d;
			if (cmd == SAMU_READ_KCAL || cmd == SAMU_READ_STEPS)
				d = EncodeDayStream(act, t, m_state.DayStartHour(), cmd == SAMU_READ_KCAL, arg);
			else
				d = EncodeMinuteStream(act, t, cmd, arg);
			size_t used = d.size();
			while (used > 0 && d[used - 1] == 0xFF)
				used--;
			std::snprintf(b, sizeof(b), "Fit Meter: %s, %u bytes asked, %zu used: ", SamuCommandName(cmd), arg, used);
			Log(b + HexBytes(d.data(), used, 32));
			if (cmd == SAMU_READ_STEPS || (cmd == SAMU_READ_KCAL && m_state.IsPet()))
			{
				// the last bank of a log receive (a pet's receive reads 02, 03, 05)
				m_state.lastSync = NowString();
				m_state.syncCount++;
				SaveState();
			}
			return d;
		}
		default:
			std::snprintf(b, sizeof(b), "Fit Meter: unknown read command %02X (arg %u): no answer", cmd, arg);
			Log(b);
			return std::nullopt;
		}
	}

	void VirtualFitMeter::HandleWrite(uint8_t cmd, uint16_t arg, const std::vector<uint8_t>& data)
	{
		char b[200];
		std::snprintf(b, sizeof(b), "Fit Meter: %s (arg %u), %zu bytes: ", SamuCommandName(cmd), arg, data.size());
		Log(b + HexBytes(data.data(), data.size(), 32));
		switch (cmd)
		{
		case SAMU_WRITE_IDENT:
			if (data.size() >= 9)
			{
				m_state.saveDataId = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
				m_state.samuId = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) | ((uint32_t)data[6] << 8) | data[7];
				m_state.identFlag = data[8];
				m_state.registered = true;
				m_state.registeredAt = NowString();
				m_state.historyStartMinute = NowMinute();
				std::snprintf(b, sizeof(b), "Fit Meter: registered (saveDataId %08X, samuId %08X%s)", m_state.saveDataId, m_state.samuId,
					m_state.identFlag == 0 ? ", pet" : "");
				Log(b);
			}
			break;
		case SAMU_WRITE_TIME:
			if (data.size() >= 6)
			{
				std::tm tm{};
				tm.tm_year = data[1] + 100;
				tm.tm_mon = data[2] - 1;
				tm.tm_mday = data[3];
				tm.tm_hour = data[4];
				tm.tm_min = data[5];
				tm.tm_isdst = -1;
				const int64_t written = (int64_t)std::mktime(&tm);
				const int64_t host = m_wallClock();
				// the meter keeps seconds itself: keep the host's seconds, take the written minute
				const int64_t hostMinute = host - (host % 60 + 60) % 60;
				m_state.clockOffsetSeconds = written - hostMinute;
				if (m_state.clockOffsetSeconds > -60 && m_state.clockOffsetSeconds < 60)
					m_state.clockOffsetSeconds = 0;
				m_state.clockSet = true;
				m_state.reportClockUnsetOnce = false;
				std::snprintf(b, sizeof(b), "Fit Meter: clock set to 20%02u-%02u-%02u %02u:%02u (offset %lld s from the host)", data[1], data[2],
					data[3], data[4], data[5], (long long)m_state.clockOffsetSeconds);
				Log(b);
			}
			break;
		case SAMU_WRITE_MII_INFO:
			m_state.miiInfo = data;
			m_state.miiName = MiiNameFromStoreData(data);
			Log("Fit Meter: Mii \"" + m_state.miiName + "\"");
			break;
		case SAMU_WRITE_PERSONAL:
			m_state.personal = data;
			break;
		case SAMU_INITIALIZE:
			Log("Fit Meter: initialized by the game (unregistered)");
			m_state.registered = false;
			m_state.saveDataId = 0xFFFFFFFF;
			m_state.samuId = 0xFFFFFFFF;
			m_state.identFlag = 0xFF;
			m_state.miiName.clear();
			m_state.miiInfo.clear();
			m_state.personal.clear();
			m_state.lastSync.clear();
			m_state.syncCount = 0;
			break;
		case SAMU_NOT_YOURS:
			break; // the real meter shows an error on its LCD
		default:
		{
			// 0x82 texts/images/sounds, 0x83 Mii icons, debug writes
			const std::string key = BlobKey(cmd, arg);
			std::error_code ec;
			fs::create_directories(fs::path(m_dir) / "blobs", ec);
			if (!WriteAll(fs::path(m_dir) / "blobs" / (key + ".bin"), data.data(), data.size()))
				Log("Fit Meter: can't write blobs/" + key + ".bin");
			m_state.blobs[key] = data;
			break;
		}
		}
		SaveState();
	}
}
