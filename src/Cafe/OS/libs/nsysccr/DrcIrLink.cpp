#include "DrcIrLink.h"
#include "FitMeterProtocol.h"

#include <cstdio>
#include <cstring>

namespace FitMeter
{
	// request layouts (big-endian, packed): __CCRCDCIRCConnect / Send / Receive / Disconnect in the game
	enum : uint8_t { CMD_CONNECT = 0, CMD_SEND = 1, CMD_RECEIVE = 2, CMD_DISCONNECT = 3 };

	void DrcIrLink::SetPeer(IrPeer* peer)
	{
		if (m_peer == peer)
			return;
		if (m_peer && m_state == State::Connected)
			m_peer->OnLinkDown();
		m_peer = peer;
		if (m_state == State::Connected)
			DropLink("peer removed");
	}

	void DrcIrLink::Reset()
	{
		if (m_state == State::Connected && m_peer)
			m_peer->OnLinkDown();
		m_state = State::Idle;
		m_toConsole.clear();
		m_pendingResult = false;
		m_eventLatched = false;
	}

	void DrcIrLink::DropLink(const char* why)
	{
		Log(std::string("IR link down (") + why + ")");
		m_state = State::Idle;
		m_toConsole.clear();
	}

	void DrcIrLink::PullPeer()
	{
		if (!m_peer || m_state != State::Connected)
			return;
		std::vector<uint8_t> payload;
		while (m_peer->PopSend(payload))
		{
			auto frame = BuildIrcFrame(m_sid, 0, payload, true);
			Log("meter -> console: " + DescribeIrcu(payload) + "   [IRC " + HexBytes(frame.data(), frame.size(), 16) + "]");
			m_toConsole.push_back(std::move(frame));
			m_framesOut++;
		}
	}

	void DrcIrLink::Update(uint64_t nowMs)
	{
		if (m_state == State::Waiting)
		{
			if (m_peer && m_peer->WantsLink(nowMs))
			{
				m_state = State::Connected;
				m_sid = (uint8_t)(m_sid % 0x7F + 1);
				Log("IR link up (the meter connected)");
				m_peer->OnLinkUp(m_connectionType, nowMs);
				PullPeer();
			}
			else if (m_deadlineMs != 0 && nowMs >= m_deadlineMs)
			{
				Log("IR connect: timeout, no meter");
				m_state = State::Idle;
				m_pendingResult = true;
				m_pendingResultCode = RESULT_TIMEOUT;
				if (m_peer)
					m_peer->OnConnectAbandoned();
			}
		}
		else if (m_state == State::Connected)
		{
			if (!m_peer)
				DropLink("no meter");
			else
				PullPeer();
		}
	}

	uint32_t DrcIrLink::GetStatus(uint64_t nowMs)
	{
		Update(nowMs);
		uint32_t s = 0;
		if (m_state == State::Connected)
			s |= STATUS_LINK;
		if (!m_toConsole.empty() || m_pendingResult)
			s |= STATUS_EVENT;
		return s;
	}

	void DrcIrLink::ClearEvent()
	{
		// the event is level-triggered here: it stays while frames are queued
	}

	static uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

	int32_t DrcIrLink::Control(uint8_t dest, const uint8_t* req, uint32_t reqSize, uint8_t* resp, uint32_t respSize, uint64_t nowMs)
	{
		if (respSize)
			std::memset(resp, 0, respSize);
		if (reqSize < 1 || respSize < 1)
			return -1;
		if (dest != 2) // CCR_CDC_DESTINATION_DRC0
		{
			char b[96];
			std::snprintf(b, sizeof(b), "IR request for destination %u (only DRC0 = 2 has an IR port here)", dest);
			Log(b);
			resp[0] = RESULT_NOT_CONNECTED;
			return 0;
		}
		Update(nowMs);
		switch (req[0])
		{
		case CMD_CONNECT:
		{
			if (reqSize < 8)
				return -1;
			const uint16_t timeout = be16(req + 1);
			const uint8_t bitrate = req[3];
			m_connectRxSize = be16(req + 4);
			const uint8_t target = req[6];
			m_connectionType = req[7];
			char b[160];
			std::snprintf(b, sizeof(b), "IR connect: timeout %u ms, bitrate %u, rxSize %u, target 0x%02X, type %u", timeout, bitrate,
				m_connectRxSize, target, m_connectionType);
			Log(b);
			if (m_state == State::Connected)
			{
				resp[0] = RESULT_OK;
				return 0;
			}
			m_toConsole.clear();
			m_pendingResult = false;
			m_state = State::Waiting;
			m_deadlineMs = timeout ? nowMs + timeout : 0;
			if (m_peer)
				m_peer->OnConnectRequested(nowMs);
			Update(nowMs);
			resp[0] = RESULT_OK;
			return 0;
		}
		case CMD_SEND:
		{
			if (reqSize < 3)
				return -1;
			const uint16_t size = be16(req + 1);
			if ((uint32_t)size + 3 > reqSize)
			{
				Log("IR send: size past the request");
				resp[0] = RESULT_NOT_CONNECTED;
				return 0;
			}
			IrcFrame frame;
			std::string err;
			if (!ParseIrcFrame(req + 3, size, frame, &err))
			{
				Log("IR send: bad frame (" + err + "): " + HexBytes(req + 3, size));
				resp[0] = RESULT_NOT_CONNECTED;
				return 0;
			}
			// the GamePad fills in the session id and the CRC (the frame's last byte)
			std::vector<uint8_t> wire(req + 3, req + 3 + size);
			if (wire.size() >= 2)
				wire[1] = m_sid;
			if (!wire.empty())
				wire.back() = Crc8(wire.data(), wire.size() - 1);
			const uint16_t hint = RxFieldToReplyPayload(frame.rxSize);
			if (m_state != State::Connected || !m_peer)
			{
				Log("console -> (no link): " + DescribeIrcu(frame.payload));
				resp[0] = RESULT_NOT_CONNECTED;
				return 0;
			}
			char b[48];
			std::snprintf(b, sizeof(b), " (reply up to %u)", hint);
			Log("console -> meter: " + DescribeIrcu(frame.payload) + b + "   [IRC " + HexBytes(wire.data(), wire.size(), 16) + "]");
			m_framesIn++;
			m_peer->OnReceive(frame.payload, hint, nowMs);
			PullPeer();
			resp[0] = RESULT_OK;
			return 0;
		}
		case CMD_RECEIVE:
		{
			if (m_pendingResult)
			{
				resp[0] = m_pendingResultCode;
				m_pendingResult = false;
				return 0;
			}
			if (m_toConsole.empty())
			{
				resp[0] = m_state == State::Connected ? RESULT_OK : RESULT_NOT_CONNECTED;
				return 0;
			}
			auto frame = std::move(m_toConsole.front());
			m_toConsole.pop_front();
			if (respSize < 3 + frame.size())
			{
				Log("IR receive: reply buffer too small");
				resp[0] = RESULT_NOT_CONNECTED;
				return 0;
			}
			resp[0] = RESULT_OK;
			resp[1] = (uint8_t)(frame.size() >> 8);
			resp[2] = (uint8_t)frame.size();
			std::memcpy(resp + 3, frame.data(), frame.size());
			return 0;
		}
		case CMD_DISCONNECT:
		{
			const bool wasConnected = m_state == State::Connected;
			const bool wasWaiting = m_state == State::Waiting;
			m_state = State::Idle;
			m_toConsole.clear();
			m_pendingResult = false;
			if (m_peer)
			{
				if (wasConnected)
					m_peer->OnLinkDown();
				else if (wasWaiting)
					m_peer->OnConnectAbandoned();
			}
			Log(wasConnected ? "IR disconnect (link closed)" : "IR disconnect");
			resp[0] = RESULT_OK;
			return 0;
		}
		default:
		{
			char b[48];
			std::snprintf(b, sizeof(b), "IR request: unknown command %u", req[0]);
			Log(b);
			resp[0] = RESULT_NOT_CONNECTED;
			return 0;
		}
		}
	}
}
