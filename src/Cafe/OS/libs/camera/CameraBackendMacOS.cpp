#include "Common/precompiled.h"
#include "CameraBackend.h"
#include "CameraCaptureMacOS.h"

namespace camera
{
	class CameraBackendMacOS : public CameraBackend
	{
	public:
		CameraBackendMacOS() : m_capture(macos::CaptureCreate()) {}
		~CameraBackendMacOS() override { macos::CaptureDestroy(m_capture); }

		bool Start() override
		{
			switch (macos::CaptureStart(m_capture))
			{
			case macos::StartResult::Started:
				cemuLog_log(LogType::Force, "Camera: capturing from \"{}\"", macos::CaptureDeviceName(m_capture));
				return true;
			case macos::StartResult::PermissionPending:
				cemuLog_log(LogType::Force, "Camera: asking for camera access (\"{}\"); the GamePad camera starts once it's granted", macos::CaptureDeviceName(m_capture));
				return true;
			case macos::StartResult::PermissionDenied:
				cemuLog_log(LogType::Force, "Camera: access denied; enable Cemu under System Settings > Privacy & Security > Camera. The GamePad camera shows no image");
				return false;
			case macos::StartResult::NoDevice:
				cemuLog_log(LogType::Force, "Camera: no camera found; the GamePad camera shows no image");
				return false;
			default:
				cemuLog_log(LogType::Force, "Camera: failed to start capture");
				return false;
			}
		}

		void Stop() override { macos::CaptureStop(m_capture); }

		bool GetLatestFrameNV12(uint8* dst, uint32 width, uint32 height, uint32 pitch) override
		{
			if (!macos::CaptureGetNV12(m_capture, dst, width, height, pitch))
				return false;
			if (!m_loggedFirstFrame)
			{
				m_loggedFirstFrame = true;
				cemuLog_log(LogType::Force, "Camera: first frame ({} from the device, delivered as {}x{} NV12, pitch {})", macos::CaptureFrameFormat(m_capture), width, height, pitch);
			}
			return true;
		}

		std::string GetDeviceName() const override { return macos::CaptureDeviceName(m_capture); }

	private:
		macos::Capture* m_capture;
		bool m_loggedFirstFrame{false};
	};

	std::unique_ptr<CameraBackend> CreateCameraBackend()
	{
		return std::make_unique<CameraBackendMacOS>();
	}
}
