#pragma once

// Maths of the Wii Balance Board, shared by the emulated board (BalanceBoardController) and the
// padscore WBC HLE. Only standard headers so that it can be built and tested on its own.
//
// The emulated board's calibration, its extension memory layout and the load cell maths are
// adapted from Dolphin's emulated balance board (dolphin-emu/dolphin PR #13337,
// Source/Core/Core/HW/WiimoteEmu/Extension/BalanceBoard.cpp, GPL-2.0-or-later).
// The raw value to kilogram conversion is a plain implementation of the board's calibration:
// three reference loads per load cell, linear interpolation between them, relative to a zero
// point measured with nothing on the board.

#include <array>
#include <cstddef>
#include <cstdint>

namespace BalanceBoardMath
{
	// Load cells, in WPADBLStatus::press[] order
	enum Sensor
	{
		kTopRight = 0,
		kBottomRight = 1,
		kTopLeft = 2,
		kBottomLeft = 3,
		kSensorCount = 4,
	};

	constexpr int kCalPointCount = 3;

	// The emulated board's calibration: the same raw value for each load cell at 0, 17 and 34 kg
	// (Dolphin's values; real boards are close to these). One raw unit is 10 grams per load cell.
	constexpr uint16_t kRawPerKg17 = 1700;
	constexpr uint16_t kCal0Kg = 10000;
	constexpr uint16_t kCal17Kg = kCal0Kg + kRawPerKg17;
	constexpr uint16_t kCal34Kg = kCal17Kg + kRawPerKg17;

	// Temperature reported by the board and stored in its calibration. While they are equal the
	// temperature correction does nothing.
	constexpr int8_t kReferenceTemperature = 0x19;
	// "always 0x69" (WiiBrew), second byte of the calibration block
	constexpr uint8_t kReferenceBattery = 0x69;
	// 1: the calibration points are 0/17/34 kg (otherwise 0/12.5/25 kg)
	constexpr uint8_t kCalibrationVersion = 1;

	// Extension memory (register block 0xA4xxxx of the board), offsets as the WPAD BLC calls use them
	constexpr uint32_t kRegCalInfo = 0x20;    // 0x01, reference battery, 0, 0
	constexpr uint32_t kRegCal0Kg = 0x24;     // 4 x u16 BE (TR, BR, TL, BL)
	constexpr uint32_t kRegCal17Kg = 0x2C;    // 4 x u16 BE
	constexpr uint32_t kRegCal34Kg = 0x34;    // 4 x u16 BE
	constexpr uint32_t kRegCalCrc = 0x3C;     // CRC-32 (BE) of the calibration
	constexpr uint32_t kRegCalTemp = 0x60;    // reference temperature, calibration version
	constexpr uint32_t kRegIdentifier = 0xFA; // 00 00 a4 20 04 02
	constexpr size_t kExtMemorySize = 0x100;
	using ExtMemory = std::array<uint8_t, kExtMemorySize>;

	// Builds the emulated board's extension memory: calibration, its CRC, temperature block, ID
	ExtMemory BuildExtensionMemory();

	// Standard reflected CRC-32 (poly 0xEDB88320, init and final xor 0xFFFFFFFF)
	uint32_t Crc32(const uint8_t* data, size_t length);
	// CRC of the calibration as the board stores it: bytes 0x24..0x3B, then 0x20, 0x21, 0x60, 0x61
	uint32_t CalibrationCrc(const ExtMemory& mem);

	// Calibration parsed from the extension memory, ready for raw -> kg conversion
	struct Calibration
	{
		bool valid = false;     // CRC matched and the points are usable
		uint32_t crc_stored = 0;
		uint32_t crc_computed = 0;
		uint16_t raw[kSensorCount][kCalPointCount]{}; // raw value at each reference load
		double load_kg[kCalPointCount]{};             // the reference loads
		int8_t ref_temperature = 0;
		uint8_t version = 0;
	};
	Calibration ParseCalibration(const ExtMemory& mem);

	// Emulated board: kilograms on one load cell -> raw value
	uint16_t KgToRaw(double kg);
	// Emulated board: raw value -> kilograms on one load cell (inverse of KgToRaw, no zero point)
	double RawToKg(uint16_t raw);

	// Raw value -> kilograms on one load cell with a calibration, relative to `zero` (the raw value
	// read with nothing on the board). Linear between the reference points: below the middle point
	// the first segment, above it the second one (also used beyond the last point).
	double RawToKg(const Calibration& cal, int sensor, uint16_t raw, double zero);

	// Sum over the load cells of the raw value above its 0 kg point
	int32_t RawLoadAboveZeroKg(const Calibration& cal, const uint16_t raw[kSensorCount]);
	// A total raw load above this (about 10 kg) means someone stands on the board
	constexpr int32_t kOnBoardRawThreshold = 1000;

	// Temperature correction of a weight: -0.7% per 10 units of temperature above the calibration's
	double TemperatureCorrected(double weight, int8_t temperature, int8_t ref_temperature);
	// Board temperature readings that mean "no valid temperature"
	constexpr bool IsInvalidTemperature(int8_t t) { return t == 127 || t == -128; }

	// Centre of balance (x: -1 left .. +1 right, y: -1 back .. +1 front) and total weight ->
	// weight on each load cell (TR, BR, TL, BL)
	std::array<double, kSensorCount> CenterOfBalanceToSensors(double x, double y, double total_weight);
	// Inverse: sensor weights -> centre of balance (0,0 if nothing on the board)
	void SensorsToCenterOfBalance(const std::array<double, kSensorCount>& sensors, double& x, double& y);

	// Distance between the load cells (cm), used to turn foot separation/length into a balance scale
	constexpr double kSensorDistanceX = 43.0;
	constexpr double kSensorDistanceY = 24.0;
	constexpr double kDefaultFootSeparation = 27.0; // cm, between the centres of the foot pads
	constexpr double kDefaultFootLength = 23.0;     // cm, length of a foot pad
	constexpr double kDefaultWeight = 60.0;         // kg
	constexpr double kMaxWeight = 250.0;            // kg
	constexpr int kDefaultBattery = 100;            // percent

	// Battery charge in percent -> raw battery byte of WPADBLStatus (Dolphin's linear fit)
	uint8_t BatteryPercentToRaw(int percent);
	// Raw battery byte -> level 0 (empty) to 4 (full)
	int BatteryRawToLevel(uint8_t raw);
}
