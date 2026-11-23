#pragma once
// A virtual Wii Fit Meter: the IRCU slave and the SAMU command handler behind the GamePad's IR link
// (DrcIrLink). It keeps what the game writes (registration: ident, time, Mii icon and info, personal data,
// the LCD texts/images/sounds) in a folder (meter.json + blobs/), and answers reads from it, from its own
// clock (host time + the offset the game's time write sets) and from a MeterDataSource (activity.json).

#include "DrcIrLink.h"
#include "FitMeterActivity.h"
#include "FitMeterDemo.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace FitMeter
{
	struct MeterState
	{
		// settings
		bool enabled = true;
		bool autoPress = true;        // "press the meter's button" whenever the game starts a connection
		int autoPressDelayMs = 1500;  // as if the player pointed the meter at the GamePad
		std::string activityFile = "activity.json"; // relative to the folder, or absolute
		std::string source = "demo";  // "demo" (the built-in 4-week cycle) or "file" (activityFile)
		int32_t stepsOverrideDay = -1; // the UI's "steps today": replaces that day's step count (any source)
		int32_t stepsOverride = 0;
		// registration (SAMU 0x80)
		bool registered = false;
		uint32_t saveDataId = 0xFFFFFFFF;
		uint32_t samuId = 0xFFFFFFFF;
		uint8_t identFlag = 0xFF;     // the byte the game wrote (0 for a pet / dog profile, FF for a person)
		// clock (SAMU 0x81)
		bool clockSet = false;
		int64_t clockOffsetSeconds = 0; // meter time = host time + offset
		// registration data (SAMU 0x84 / 0x85)
		std::vector<uint8_t> miiInfo;
		std::vector<uint8_t> personal;
		std::string miiName;
		// 0x82 / 0x83 / debug writes, by "<cmd>_<arg>" (hex)
		std::map<std::string, std::vector<uint8_t>> blobs;
		// bookkeeping
		std::string registeredAt;
		int64_t historyStartMinute = INT64_MIN; // meter clock, absolute minute: no data before it (registration)
		int pretendDaysAgo = 0;       // test option: the data starts this many days before historyStartMinute
		bool reportClockUnsetOnce = false; // the next time read says "not set": the game then asks the full 7 days
		std::string lastSync;
		uint32_t syncCount = 0;

		bool IsPet() const { return registered && identFlag == 0; }
		int DayStartHour() const; // 3 with Wii Fit U's night-owl setting (personal data byte 3), else 0

		bool Load(const std::string& dir, std::string& error);
		bool Save(const std::string& dir, std::string& error) const;
	};

	std::string MiiNameFromStoreData(const std::vector<uint8_t>& data);

	class VirtualFitMeter : public IrPeer
	{
	public:
		using WallClock = std::function<int64_t()>; // seconds since 1970 (UTC)

		explicit VirtualFitMeter(std::string dir);

		void SetLogger(std::function<void(const std::string&)> log) { m_log = std::move(log); }
		void SetWallClock(WallClock clock) { m_wallClock = std::move(clock); }
		void SetDataSource(std::shared_ptr<MeterDataSource> source) { m_source = std::move(source); }
		void UseSourceFromState(); // the demo or the file, as m_state.source says
		// the data the meter sends now: the source's, plus the steps-today override
		bool GetActivity(Activity& out, std::string& error);
		int32_t LogicalToday() const; // today by the meter's clock and day start (night owl: before 3:00 = yesterday)
		std::shared_ptr<MeterDataSource> GetDataSource() const { return m_source; }

		MeterState& State() { return m_state; }
		const MeterState& State() const { return m_state; }
		const std::string& GetDir() const { return m_dir; }
		std::string ActivityPath() const;
		bool SaveState();
		void Unregister();          // the UI's "Reset meter": back to a factory-new meter
		void ClearHistory();        // the UI's "Clear meter history": no data before now (keeps the registration)
		int64_t NowMinute() const;  // the meter's clock as an absolute minute
		int64_t DataStartMinute() const; // where the meter's data starts (history start - the test option's days)
		void PressButton(uint64_t nowMs); // the UI's button: try to link for the next few seconds

		MeterTime Now() const;      // the meter's clock
		bool IsLinked() const { return m_linked; }
		bool IsConnectRequested(uint64_t nowMs) const; // the game is waiting for the meter
		const std::string& LastError() const { return m_lastError; }

		// IrPeer
		void OnConnectRequested(uint64_t nowMs) override;
		void OnConnectAbandoned() override;
		bool WantsLink(uint64_t nowMs) override;
		void OnLinkUp(uint8_t connectionType, uint64_t nowMs) override;
		void OnLinkDown() override;
		void OnReceive(const std::vector<uint8_t>& payload, uint16_t replyHint, uint64_t nowMs) override;
		bool PopSend(std::vector<uint8_t>& payload) override;

		// the SAMU layer without the transport (plain data, no XOR): used by the transport and the tests
		std::optional<std::vector<uint8_t>> HandleRead(uint8_t cmd, uint16_t arg);
		void HandleWrite(uint8_t cmd, uint16_t arg, const std::vector<uint8_t>& data);

	private:
		enum class Xfer { Idle, WriteData, ReadData, ReadWaitLastAck };

		void Send(std::vector<uint8_t> payload);
		void SendNextChunk(uint16_t replyHint);
		void Log(const std::string& s) { if (m_log) m_log(s); }
		std::string NowString() const;

		std::string m_dir;
		MeterState m_state;
		std::shared_ptr<MeterDataSource> m_source;
		WallClock m_wallClock;
		std::function<void(const std::string&)> m_log;
		std::string m_lastError;

		// link
		bool m_connectRequested = false;
		uint64_t m_connectRequestedMs = 0;
		uint64_t m_connectActivityMs = 0;
		uint64_t m_buttonPressedMs = 0;
		bool m_buttonPressed = false;
		bool m_linked = false;

		// IRCU slave
		Xfer m_xfer = Xfer::Idle;
		uint8_t m_cmd = 0;
		uint16_t m_arg = 0;
		std::vector<uint8_t> m_buffer;   // write data received / read data to send (on the wire: XORed)
		size_t m_readPos = 0;
		std::vector<uint8_t> m_lastSent; // for F5
		std::deque<std::vector<uint8_t>> m_out;
	};
}
