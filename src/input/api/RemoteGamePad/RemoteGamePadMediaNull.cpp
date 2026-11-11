// Remote GamePad media on platforms without VideoToolbox: no media port (HELLO_ACK announces 0),
// the camera and mic fall back to the host devices.
#include "input/api/RemoteGamePad/RemoteGamePadMedia.h"

namespace RemoteGamePad::Media
{
	namespace
	{
		CameraSource s_camera = CameraSource::Auto;
		MicSource s_mic = MicSource::Auto;
		SpeakerTarget s_speaker = SpeakerTarget::Phone;
		VideoResolution s_video = VideoResolution::GamePad;
	}
	bool IsSupported() { return false; }
	bool Start(uint16_t, std::string& error) { error = "not supported on this platform"; return false; }
	uint16_t Port() { return 0; }
	void SetClient(uint32_t, uint16_t, const std::string&) {}
	void ClearClient() {}
	std::string StatusText() { return {}; }
	void SetCameraSource(CameraSource s) { s_camera = s; }
	CameraSource GetCameraSource() { return s_camera; }
	void SetMicSource(MicSource s) { s_mic = s; }
	MicSource GetMicSource() { return s_mic; }
	void SetSpeakerTarget(SpeakerTarget t) { s_speaker = t; }
	SpeakerTarget GetSpeakerTarget() { return s_speaker; }
	bool SubmitPadAudio(const int16_t*, size_t, size_t) { return false; }
	bool VideoWanted() { return false; }
	void DebugRestartVideoStream() {}
	void SetVideoResolution(VideoResolution r) { s_video = r; }
	VideoResolution GetVideoResolution() { return s_video; }
	void SetVideoSourceSize(int, int) {}
	void GetVideoBufferSize(void*, int& w, int& h) { w = 854; h = 480; }
	void* AcquireVideoBuffer() { return nullptr; }
	void SubmitVideoBuffer(void*, uint64_t) {}
	void ReleaseVideoBuffer(void*) {}
	void* GetIOSurface(void*) { return nullptr; }
	uint64_t NowUs() { return 0; }
	bool PhoneCameraSelected() { return false; }
	void SetCameraWanted(bool) {}
	bool GetCameraFrameNV12(uint8_t*, uint32_t, uint32_t, uint32_t) { return false; }
	bool PhoneMicSelected() { return false; }
	void SetMicWanted(bool) {}
	bool ReadMic(int16_t*, size_t) { return false; }
}
