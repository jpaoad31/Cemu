#pragma once

#include "input/emulated/BalanceBoardMath.h"

// https://wiibrew.org/wiki/Wiimote

enum InputReportId : uint8
{
	kNone = 0,

	kStatus = 0x20,
	kRead = 0x21,
	kAcknowledge = 0x22,

	kDataCore = 0x30,
	kDataCoreAcc = 0x31,
	kDataCoreExt8 = 0x32,
	kDataCoreAccIR = 0x33,
	kDataCoreExt19 = 0x34,
	kDataCoreAccExt = 0x35,
	kDataCoreIRExt = 0x36,
	kDataCoreAccIRExt = 0x37,
	kDataExt = 0x3d,
};

enum RegisterAddress : uint32
{
	kRegisterCalibration = 0x16,
	kRegisterCalibration2 = 0x20, // backup calibration data

	kRegisterIR = 0x4b00030,
	kRegisterIRSensitivity1 = 0x4b00000,
	kRegisterIRSensitivity2 = 0x4b0001a,
	kRegisterIRMode = 0x4b00033,

	kRegisterExtensionEncrypted = 0x4a40040,

	kRegisterExtension1 = 0x4a400f0,
	kRegisterExtension2 = 0x4a400fb,
	kRegisterExtensionType = 0x4a400fa,
	kRegisterExtensionCalibration = 0x4a40020,
	kRegisterBalanceBoardCalibration = 0x4a40020, // 0x20 bytes: info, 0/17/34 kg per load cell, CRC
	kRegisterBalanceBoardCalibration2 = 0x4a40060, // 2 bytes: reference temperature, version
	kRegisterBalanceBoardControl = 0x4a400f1, // WPADControlBLC commands

	// speaker (0xA20000-0xA20009)
	kRegisterSpeakerConfig = 0x4a20001, // 7 bytes: 0x00, format, rate (u16 LE), volume, 0x0C, 0x0E
	kRegisterSpeakerPlay = 0x4a20008,
	kRegisterSpeakerEnable = 0x4a20009,

	kRegisterMotionPlusDetect = 0x4a600fa,
	kRegisterMotionPlusInit = 0x4a600f0,
	kRegisterMotionPlusEnable = 0x4a600fe,
};

enum ExtensionType : uint64
{
	kExtensionNunchuck = 0x0000A4200000,
	kExtensionClassic = 0x0000A4200101,
	kExtensionClassicPro = 0x0100A4200101,
	kExtensionDrawsome = 0xFF00A4200013,
	kExtensionGuitar = 0x0000A4200103,
	kExtensionDrums = 0x0100A4200103,
	kExtensionBalanceBoard = 0x0000A4200402, // 00 00 a4 20 04 02, permanently attached to the board

	kExtensionMotionPlusInactive = 0xa4200005,
	kExtensionMotionPlus = 0xa6200005,

	kExtensionPartialyInserted = 0xffffffffffff,
};

enum MemoryType : uint8
{
	kEEPROMMemory = 0,
	kRegisterMemory = 0x4,
};

enum StatusBitmask : uint8
{
	kBatteryEmpty = 0x1,
	kExtensionConnected = 0x2,
	kSpeakerEnabled = 0x4,
	kIREnabled = 0x8,
	kLed1 = 0x10,
	kLed2 = 0x20,
	kLed3 = 0x40,
	kLed4 = 0x80
};

enum OutputReportId : uint8
{
	kLED = 0x11,
	kType = 0x12,
	kIR = 0x13,
	kSpeakerState = 0x14,
	kStatusRequest = 0x15,
	kWriteMemory = 0x16,
	kReadMemory = 0x17,
	kSpeakerData = 0x18,
	kSpeakerMute = 0x19,
	kIR2 = 0x1A,
};

enum IRMode : uint8
{
	kIRDisabled,
	kBasicIR = 1,
	kExtendedIR = 3,
	kFullIR = 5,
};

enum WiimoteButtons
{
	kWiimoteButton_Left = 0,
	kWiimoteButton_Right = 1,
	kWiimoteButton_Down = 2,
	kWiimoteButton_Up = 3,
	kWiimoteButton_Plus = 4,

	kWiimoteButton_Two = 8,
	kWiimoteButton_One = 9,
	kWiimoteButton_B = 10,
	kWiimoteButton_A = 11,
	kWiimoteButton_Minus = 12,

	kWiimoteButton_Home = 15,

	// self defined
	kWiimoteButton_C = 16,
	kWiimoteButton_Z = 17,

	kHighestWiimote = 20,
};

enum ClassicButtons
{
	kClassicButton_R = 1,
	kClassicButton_Plus = 2,
	kClassicButton_Home = 3,
	kClassicButton_Minus = 4,
	kClassicButton_L = 5,
	kClassicButton_Down = 6,
	kClassicButton_Right = 7,
	kClassicButton_Up = 8,
	kClassicButton_Left = 9,
	kClassicButton_ZR = 10,
	kClassicButton_X = 11,
	kClassicButton_A = 12,
	kClassicButton_Y = 13,
	kClassicButton_B = 14,
	kClassicButton_ZL = 15,
};

struct Calibration
{
	glm::vec<3, uint16> zero{ 0x200, 0x200, 0x200 };
	glm::vec<3, uint16> gravity{ 0x240, 0x240, 0x240 };
};

struct BasicIR
{
	uint8 x1;
	uint8 y1;

	struct
	{
		uint8 x2 : 2;
		uint8 y2 : 2;
		uint8 x1 : 2;
		uint8 y1 : 2;
	} bits;
	static_assert(sizeof(bits) == 1);

	uint8 x2;
	uint8 y2;
};
static_assert(sizeof(BasicIR) == 5);

struct ExtendedIR
{
	uint8 x;
	uint8 y;
	struct
	{
		uint8 size : 4;
		uint8 x : 2;
		uint8 y : 2;
	} bits;
	static_assert(sizeof(bits) == 1);
};
static_assert(sizeof(ExtendedIR) == 3);

struct IRDot
{
	bool visible = false;
	glm::vec2 pos;
	glm::vec<2, uint16> raw;
	uint32 size;
};

struct IRCamera
{
	IRMode mode;
	std::array<IRDot, 4> dots{}, prev_dots{};

	glm::vec2 position, m_prev_position;
	glm::vec2 middle;
	float distance;
	std::pair<sint32, sint32> indices{ 0,1 };
};

struct NunchuchCalibration : Calibration
{
	glm::vec<2, uint8> min{};
	glm::vec<2, uint8> center{ 0x7f, 0x7f };
	glm::vec<2, uint8> max{ 0xff, 0xff };
};

struct MotionPlusData
{
	Calibration calibration{};

	glm::vec3 orientation; // yaw, roll, pitch
	bool slow_roll = false;
	bool slow_pitch = false;
	bool slow_yaw = false;
	bool extension_connected = false;
};

struct NunchuckData
{
	glm::vec3 acceleration{}, prev_acceleration{};
	NunchuchCalibration calibration{};

	bool c = false;
	bool z = false;

	glm::vec2 axis{};
	glm::vec<2, uint8> raw_axis{};

	MotionSample motion_sample{};
};

struct ClassicData
{
	glm::vec2 left_axis{};
	glm::vec<2, uint8> left_raw_axis{};

	glm::vec2 right_axis{};
	glm::vec<2, uint8> right_raw_axis{};

	glm::vec2 trigger{};
	glm::vec<2, uint8> raw_trigger{};
	uint16 buttons = 0;
};

// Real Wii Balance Board: a Wii Remote (057e:0306, "Nintendo RVL-WBC-01") with a permanently
// attached extension 00 00 a4 20 04 02. Its extension registers hold the calibration (0x20..0x3F,
// 0x60..0x61, see BalanceBoardMath.h) and report 0x32/0x34 carries 11 bytes of extension data:
// TR, BR, TL, BL (u16 big-endian load cells), temperature, padding, battery.
// Protocol as in Dolphin's balance board code (WiimoteReal.cpp IsBalanceBoard, WiimoteEmu
// Extension/BalanceBoard.cpp; GPL-2.0-or-later).
struct BalanceBoardData
{
	// the board's extension memory as read from it (calibration blocks and the identifier)
	BalanceBoardMath::ExtMemory memory{};
	bool received_cal_0x20 = false;
	bool received_cal_0x30 = false;
	bool received_cal_0x60 = false;
	bool calibration_logged = false;
	bool calibration_valid = false; // CRC matched

	std::array<uint16, BalanceBoardMath::kSensorCount> raw{}; // TR, BR, TL, BL
	uint8 temperature = 0;
	uint8 battery = 0; // extension battery byte (WPADBLStatus::battery)
	uint32 report_count = 0;

	bool has_calibration() const { return received_cal_0x20 && received_cal_0x30 && received_cal_0x60; }
	bool has_data() const { return report_count > 0; }
};
