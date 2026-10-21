#pragma once

// Host camera for the GamePad camera (camera.rpl HLE).
// A backend captures frames from a host device and hands out the latest one
// converted to the Wii U's camera format: NV12, 640x480 visible, with the
// pitch the game's surface uses (768 on hardware).

namespace camera
{
	class CameraBackend
	{
	public:
		virtual ~CameraBackend() = default;

		// start or stop capturing; Start() may return true before the first
		// frame arrives (permission prompts and device start-up are async)
		virtual bool Start() = 0;
		virtual void Stop() = 0;

		// copy the latest frame into dst as NV12: a Y plane of height rows
		// followed by an interleaved UV plane of height/2 rows, both with the
		// given pitch. Returns false if no frame has been captured yet
		virtual bool GetLatestFrameNV12(uint8* dst, uint32 width, uint32 height, uint32 pitch) = 0;

		// host device name, for the log
		virtual std::string GetDeviceName() const = 0;
	};

	// the platform's backend, or nullptr if the platform has none
	std::unique_ptr<CameraBackend> CreateCameraBackend();
}
