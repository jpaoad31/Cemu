#include "Common/precompiled.h"
#include "CameraBackend.h"
#include "CameraCaptureMacOS.h"
#include "input/api/RemoteGamePad/RemoteGamePadMedia.h"

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

	// The GamePad camera: a connected Remote GamePad phone's front camera when there is one (and
	// the input settings allow it), else the Mac's camera. The Mac camera is only started while
	// it's actually the source, so the phone being there doesn't light up the Mac's camera.
	class CameraBackendSwitching : public CameraBackend
	{
	public:
		bool Start() override
		{
			m_running = true;
			RemoteGamePad::Media::SetCameraWanted(true);
			m_mac_failed = false;
			// running either way: a phone may connect later, and no picture means black frames
			if (!RemoteGamePad::Media::PhoneCameraSelected())
			{
				m_mac_failed = !StartMac();
				return true;
			}
			cemuLog_log(LogType::Force, "Camera: using the Remote GamePad phone's front camera");
			return true;
		}

		void Stop() override
		{
			m_running = false;
			RemoteGamePad::Media::SetCameraWanted(false);
			StopMac();
		}

		bool GetLatestFrameNV12(uint8* dst, uint32 width, uint32 height, uint32 pitch) override
		{
			const bool phone = RemoteGamePad::Media::PhoneCameraSelected();
			if (phone != m_last_phone)
			{
				m_last_phone = phone;
				m_mac_failed = false;
				cemuLog_log(LogType::Force, "Camera: source is now {}", phone ? "the Remote GamePad phone" : "the Mac camera");
			}
			if (phone)
			{
				// the Mac camera goes off once the phone has delivered for a while
				if (RemoteGamePad::Media::GetCameraFrameNV12(dst, width, height, pitch))
				{
					if (m_mac_started && ++m_phone_frames > 60)
						StopMac();
					return true;
				}
				// phone selected but no picture (yet): bridge with the Mac camera if it's running
				return m_mac_started && m_mac.GetLatestFrameNV12(dst, width, height, pitch);
			}
			m_phone_frames = 0;
			if (!m_mac_started && m_running && !m_mac_failed)
				m_mac_failed = !StartMac();
			return m_mac_started && m_mac.GetLatestFrameNV12(dst, width, height, pitch);
		}

		std::string GetDeviceName() const override
		{
			return RemoteGamePad::Media::PhoneCameraSelected() ? std::string("Remote GamePad phone") : m_mac.GetDeviceName();
		}

	private:
		bool StartMac()
		{
			if (!m_mac_started)
				m_mac_started = m_mac.Start();
			return m_mac_started;
		}
		void StopMac()
		{
			if (m_mac_started)
				m_mac.Stop();
			m_mac_started = false;
		}

		CameraBackendMacOS m_mac;
		bool m_running = false, m_mac_started = false, m_mac_failed = false, m_last_phone = false;
		uint32 m_phone_frames = 0;
	};

	std::unique_ptr<CameraBackend> CreateCameraBackend()
	{
		return std::make_unique<CameraBackendSwitching>();
	}
}
