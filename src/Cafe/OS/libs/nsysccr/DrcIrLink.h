#pragma once
// The GamePad's (DRC's) IR engine as nsysccr.CCRCDCPerIrdaControl and vpadbase.VPADBASEGetIRCStatus /
// VPADBASEClearIRCEvent show it to a game, with a pluggable peer on the other end of the IR link.
// Plain C++: the HLE wrapper (nsysccr.cpp) locks and passes the guest buffers; the test drives it directly.

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace FitMeter
{
	// the device on the far side of the IR link (the virtual meter now; a real meter through a bridge later)
	class IrPeer
	{
	public:
		virtual ~IrPeer() = default;
		// the console started a connection (the game shows "point the meter at the GamePad")
		virtual void OnConnectRequested(uint64_t nowMs) {}
		// the console's connection attempt ended without a link (timeout or disconnect)
		virtual void OnConnectAbandoned() {}
		// is the peer trying to link now (the meter's button pressed while pointed at the GamePad)
		virtual bool WantsLink(uint64_t nowMs) = 0;
		// connectionType: the CONNECT request's type (0 = wait for the peer, as Wii Fit U uses it)
		virtual void OnLinkUp(uint8_t connectionType, uint64_t nowMs) = 0;
		virtual void OnLinkDown() = 0;
		// an IRC payload from the console; replyHint: the reply payload size the console expects
		virtual void OnReceive(const std::vector<uint8_t>& payload, uint16_t replyHint, uint64_t nowMs) = 0;
		// the next payload for the console, if any
		virtual bool PopSend(std::vector<uint8_t>& payload) = 0;
	};

	class DrcIrLink
	{
	public:
		// VPADBASEGetIRCStatus bits (IRCProc's reading)
		static constexpr uint32_t STATUS_EVENT = 0x1; // a frame (or a result) waits for RECEIVE
		static constexpr uint32_t STATUS_LINK = 0x2;  // the IR link is up
		// RECEIVE result codes (IRCU's __IRCUConvertError turns 1 into its timeout error 23)
		static constexpr uint8_t RESULT_OK = 0;
		static constexpr uint8_t RESULT_TIMEOUT = 1;
		static constexpr uint8_t RESULT_NOT_CONNECTED = 2;

		void SetPeer(IrPeer* peer);
		void SetLogger(std::function<void(const std::string&)> log) { m_log = std::move(log); }

		// CCRCDCPerIrdaControl(dest, req, reqSize, resp, respSize): returns the call's own result (0 = done);
		// the IR result is resp[0]
		int32_t Control(uint8_t dest, const uint8_t* req, uint32_t reqSize, uint8_t* resp, uint32_t respSize, uint64_t nowMs);
		uint32_t GetStatus(uint64_t nowMs);
		void ClearEvent();

		enum class State { Idle, Waiting, Connected };
		State GetState() const { return m_state; }
		uint64_t FramesIn() const { return m_framesIn; }
		uint64_t FramesOut() const { return m_framesOut; }
		void Reset();

	private:
		void Update(uint64_t nowMs);
		void PullPeer();
		void Log(const std::string& s) { if (m_log) m_log(s); }
		void DropLink(const char* why);

		IrPeer* m_peer = nullptr;
		std::function<void(const std::string&)> m_log;
		State m_state = State::Idle;
		uint64_t m_deadlineMs = 0;      // 0: no timeout
		uint8_t m_connectionType = 0;
		uint16_t m_connectRxSize = 0;
		std::deque<std::vector<uint8_t>> m_toConsole; // complete IRC frames (with CRC)
		bool m_pendingResult = false;   // a RECEIVE result without a frame (timeout)
		uint8_t m_pendingResultCode = 0;
		bool m_eventLatched = false;
		uint8_t m_sid = 0x01;           // the session id the GamePad gives the link
		uint64_t m_framesIn = 0, m_framesOut = 0;
	};
}
