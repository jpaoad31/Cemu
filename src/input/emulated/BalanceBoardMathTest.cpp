// Standalone check of BalanceBoardMath (not part of the CMake build: Cemu has no test target).
// Build and run from the repository root:
//   clang++ -std=c++20 -Wall -I src src/input/emulated/BalanceBoardMathTest.cpp src/input/emulated/BalanceBoardMath.cpp -o /tmp/bbtest && /tmp/bbtest
#include "input/emulated/BalanceBoardMath.h"
#include <cstdio>
#include <cmath>
#include <cstring>
using namespace BalanceBoardMath;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
static bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

// independent table-driven CRC-32 (zlib style)
static uint32_t crc_table(const uint8_t* p, size_t n) {
	static uint32_t t[256]; static bool init = false;
	if (!init) { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; } init = true; }
	uint32_t c = 0xFFFFFFFFu; while (n--) c = t[(c ^ *p++) & 0xFF] ^ (c >> 8); return c ^ 0xFFFFFFFFu;
}

int main() {
	ExtMemory mem = BuildExtensionMemory();
	// identifier
	const uint8_t id[6] = {0x00, 0x00, 0xa4, 0x20, 0x04, 0x02};
	CHECK(memcmp(&mem[kRegIdentifier], id, 6) == 0);
	// calibration block bytes
	CHECK(mem[0x20] == 0x01 && mem[0x21] == 0x69 && mem[0x22] == 0 && mem[0x23] == 0);
	CHECK(mem[0x24] == 0x27 && mem[0x25] == 0x10); // 10000
	CHECK(mem[0x2C] == 0x2D && mem[0x2D] == 0xB4); // 11700
	CHECK(mem[0x34] == 0x34 && mem[0x35] == 0x58); // 13400
	CHECK(mem[0x60] == 0x19 && mem[0x61] == 0x01);
	// CRC against an independent CRC over the same bytes
	uint8_t buf[28]; memcpy(buf, &mem[0x24], 24); buf[24] = mem[0x20]; buf[25] = mem[0x21]; buf[26] = mem[0x60]; buf[27] = mem[0x61];
	const uint32_t crc = (uint32_t)mem[0x3C] << 24 | mem[0x3D] << 16 | mem[0x3E] << 8 | mem[0x3F];
	CHECK(crc == crc_table(buf, 28));
	CHECK(Crc32((const uint8_t*)"123456789", 9) == 0xCBF43926u); // standard check value
	printf("calibration CRC %08x, block 0x20: ", crc); for (int i = 0x20; i < 0x40; i++) printf("%02x", mem[i]); printf("\n");

	Calibration cal = ParseCalibration(mem);
	CHECK(cal.valid);
	for (int s = 0; s < 4; s++) CHECK(cal.raw[s][0] == 10000 && cal.raw[s][1] == 11700 && cal.raw[s][2] == 13400);
	CHECK(near(cal.load_kg[1], 17) && near(cal.load_kg[2], 34) && cal.ref_temperature == 0x19);

	// a broken CRC is detected
	ExtMemory bad = mem; bad[0x25] ^= 1; CHECK(!ParseCalibration(bad).valid);
	bad = mem; bad[0x60] ^= 1; CHECK(!ParseCalibration(bad).valid);

	// kg <-> raw round trip, per sensor, and against the SDK form
	for (double kg = 0; kg <= 60; kg += 0.5) {
		uint16_t raw = KgToRaw(kg);
		CHECK(near(RawToKg(raw), kg, 0.006));
		for (int s = 0; s < 4; s++) {
			const double mine = RawToKg(cal, s, raw, 10000.0);
			CHECK(near(mine, kg, 0.006));
		}
	}
	// a zero point away from the factory 0 kg point shifts the result
	CHECK(near(RawToKg(cal, 0, 11500, 10050.0), 14.5));

	// 60 kg centred: 15 kg and raw 11500 per load cell, total load 6000 (on the board)
	auto w = CenterOfBalanceToSensors(0, 0, 60);
	uint16_t raw[4]; double sum = 0;
	for (int s = 0; s < 4; s++) { raw[s] = KgToRaw(w[s]); CHECK(raw[s] == 11500); sum += RawToKg(cal, s, raw[s], 10000); }
	CHECK(near(sum, 60, 1e-9));
	CHECK(RawLoadAboveZeroKg(cal, raw) == 6000 && RawLoadAboveZeroKg(cal, raw) > kOnBoardRawThreshold);
	// stepped off: 0 kg, raw at the 0 kg point, below the threshold
	w = CenterOfBalanceToSensors(0, 0, 0);
	for (int s = 0; s < 4; s++) raw[s] = KgToRaw(w[s]);
	CHECK(raw[0] == 10000 && RawLoadAboveZeroKg(cal, raw) == 0);
	// 100 kg: sensors above the 17 kg point use the second segment
	w = CenterOfBalanceToSensors(0, 0, 100); sum = 0;
	for (int s = 0; s < 4; s++) { raw[s] = KgToRaw(w[s]); sum += RawToKg(cal, s, raw[s], 10000); }
	CHECK(raw[0] == 12500 && near(sum, 100, 1e-9));

	// centre of balance: full right deflection with default feet = 27/43, total kept
	const double x = 1.0 * kDefaultFootSeparation / kSensorDistanceX, y = -1.0 * kDefaultFootLength / kSensorDistanceY;
	w = CenterOfBalanceToSensors(x, y, 80);
	double bx, by; SensorsToCenterOfBalance(w, bx, by);
	CHECK(near(bx, x) && near(by, y) && near(w[0] + w[1] + w[2] + w[3], 80));
	CHECK(w[kTopRight] < w[kBottomRight] && w[kTopLeft] < w[kTopRight]);
	for (double v : w) CHECK(v >= 0);
	printf("80 kg, balance (%.3f, %.3f): TR %.2f BR %.2f TL %.2f BL %.2f\n", x, y, w[0], w[1], w[2], w[3]);

	// temperature: no change at the reference temperature, -0.7%% per 10 units above
	CHECK(near(TemperatureCorrected(60, 0x19, 0x19), 60));
	CHECK(near(TemperatureCorrected(60, 0x19 + 10, 0x19), 60 * 0.993));
	CHECK(IsInvalidTemperature(127) && IsInvalidTemperature(-128) && !IsInvalidTemperature(0x19));

	// battery
	CHECK(BatteryPercentToRaw(100) == 0x88 && BatteryRawToLevel(BatteryPercentToRaw(100)) == 4);
	CHECK(BatteryRawToLevel(BatteryPercentToRaw(50)) == 2);
	CHECK(BatteryRawToLevel(0x60) == 0);
	printf("battery 100%% -> 0x%02x level %d, 50%% -> 0x%02x level %d, 0%% -> 0x%02x level %d\n",
		BatteryPercentToRaw(100), BatteryRawToLevel(BatteryPercentToRaw(100)), BatteryPercentToRaw(50), BatteryRawToLevel(BatteryPercentToRaw(50)),
		BatteryPercentToRaw(0), BatteryRawToLevel(BatteryPercentToRaw(0)));

	printf(fails ? "%d FAILED\n" : "all checks passed\n", fails);
	return fails != 0;
}
