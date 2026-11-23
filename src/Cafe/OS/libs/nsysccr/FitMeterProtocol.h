#pragma once
// The Wii Fit Meter's link over the GamePad's IR port, as Wii Fit U drives it (see FIT_METER.md).
// Plain C++ (std only): built into Cemu and into the standalone test (FitMeterTest.cpp).
//
// Layers, bottom to top:
//   IRC   frame  A5 sid flags [len] rxSize16 payload crc8    (the GamePad adds the CRC and the session id)
//   IRCU  payload F4 cmd argHi argLo | F0/F1 data... | F2 | F3 | F5
//   SAMU  the F4 command byte + the data (every data byte XOR 0xAA)

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace FitMeter
{
	constexpr uint8_t SAMU_XOR = 0xAA;

	// IRCU packet types (first payload byte)
	constexpr uint8_t IRCU_DATA = 0xF0;       // a data chunk, more follow
	constexpr uint8_t IRCU_DATA_LAST = 0xF1;  // the last data chunk
	constexpr uint8_t IRCU_ACK = 0xF2;
	constexpr uint8_t IRCU_ACK_LAST = 0xF3;   // end of a transfer; also the meter's "ready" after the link comes up
	constexpr uint8_t IRCU_COMMAND = 0xF4;    // F4 cmd argHi argLo (cmd bit 7: the console writes)
	constexpr uint8_t IRCU_RESEND = 0xF5;
	constexpr size_t IRCU_MAX_CHUNK = 509;    // IRC payload max 510 (IRCProc), minus the F0/F1 byte

	// SAMU commands
	enum : uint8_t
	{
		SAMU_READ_IDENT = 0x00,     // 9: saveDataId u32, samuId u32, flag (0 = registered)
		SAMU_READ_TIME = 0x01,      // 6: flag (0 = clock set), year-2000, month 1-12, day, hour, minute
		SAMU_READ_METS = 0x02,      // per minute, newest first, u8 stream (arg = bytes wanted)
		SAMU_READ_ALTITUDE = 0x03,  // per minute altitude change, newest first
		SAMU_READ_TAG = 0x04,       // per minute activity tag, newest first
		SAMU_READ_KCAL = 0x05,      // per day, newest first, u16 BE (120 bytes)
		SAMU_READ_STEPS = 0x06,     // per day, newest first, u16 BE (120 bytes)
		SAMU_WRITE_IDENT = 0x80,
		SAMU_WRITE_TIME = 0x81,
		SAMU_WRITE_LOCALIZE = 0x82, // arg = bank (82_<bank>.dat of content/samu/*.jarc)
		SAMU_WRITE_MII_ICON = 0x83, // arg = icon 0..7
		SAMU_WRITE_MII_INFO = 0x84, // FFLStoreData (96) + 6 zero bytes
		SAMU_WRITE_PERSONAL = 0x85,
		SAMU_INITIALIZE = 0xF0,     // one byte: unregister (the game's "initialize the Fit Meter")
		SAMU_NOT_YOURS = 0xF2,      // one byte: "this isn't your Fit Meter" / not registered
	};

	// CRC-8, polynomial 0x07, initial value 0, over every byte before it (inferred; the game never checks it)
	uint8_t Crc8(const uint8_t* data, size_t size);

	void XorSamu(std::vector<uint8_t>& data);

	struct IrcFrame
	{
		uint8_t sid = 0;
		bool control = false;
		uint16_t rxSize = 0;           // the frame's "expected reply" field as sent
		std::vector<uint8_t> payload;  // after rxSize
	};

	// a frame without the CRC (crc = true appends it, as the GamePad does)
	std::vector<uint8_t> BuildIrcFrame(uint8_t sid, uint16_t rxSize, const std::vector<uint8_t>& payload, bool crc);
	// parses the frame at data[0] (A5 ...); size is what's available (padding after the frame is ignored)
	bool ParseIrcFrame(const uint8_t* data, size_t size, IrcFrame& out, std::string* error = nullptr);
	// the console's rxSize field (= reply payload size + 6, or + 7 above 61) -> the IRC payload size it expects
	uint16_t RxFieldToReplyPayload(uint16_t rxField);
	// the inverse (what __CCRCDCIRCSend puts in the field for an expected reply size)
	uint16_t ReplyPayloadToRxField(uint16_t replyPayload);

	std::string HexBytes(const uint8_t* data, size_t size, size_t max = 48);
	const char* SamuCommandName(uint8_t cmd);
	// one line for the log: the IRCU packet and, for F4, the SAMU command
	std::string DescribeIrcu(const std::vector<uint8_t>& payload);
}
