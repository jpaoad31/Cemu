#pragma once

// Remote GamePad media (protocol 1.2): the GamePad (DRC) video to the phone, the phone's front
// camera as the GamePad camera and its microphone as the GamePad mic. All of it runs over one UDP
// "media port" (the control port + 1, dual-stack), announced in HELLO_ACK.video_port.
// macOS only (VideoToolbox); elsewhere these are stubs and HELLO_ACK announces no media port.
//
// Who calls what:
//  - the controller provider: Start(port) once, SetClient()/ClearClient() as phones come and go
//  - the Metal renderer: VideoWanted() each DRC frame, then AcquireVideoBuffer() -> render into
//    it on the GPU -> SubmitVideoBuffer() from the command buffer's completion handler
//  - camera.rpl's backend: CameraWanted()/GetCameraFrameNV12()
//  - mic.rpl: SetMicWanted()/ReadMic()

#include <cstdint>
#include <string>

namespace RemoteGamePad::Media
{
	enum class CameraSource : int
	{
		Auto = 0, // the phone's front camera while a phone that has one is connected, else the Mac's
		Mac = 1,  // always the Mac's camera
	};
	enum class MicSource : int
	{
		Auto = 0, // the phone's microphone while a phone that has one is connected
		Host = 1, // Cemu's audio input device (General settings)
	};

	// provider side
	bool IsSupported();                       // false on platforms without the media path
	bool Start(uint16_t port, std::string& error); // binds the media port (idempotent)
	uint16_t Port();                          // bound media port, 0 if none
	void SetClient(uint32_t client_id, uint16_t capabilities, const std::string& name);
	void ClearClient();
	std::string StatusText();                 // one or two lines for Cemu's input settings

	void SetCameraSource(CameraSource s);
	CameraSource GetCameraSource();
	void SetMicSource(MicSource s);
	MicSource GetMicSource();

	// video (renderer)
	bool VideoWanted();                       // a phone is subscribed: capture the DRC
	void GetVideoSize(int& width, int& height); // 854x480
	// an IOSurface-backed BGRA CVPixelBufferRef (retained), nullptr if the encoder is busy
	void* AcquireVideoBuffer();
	// hands a buffer from AcquireVideoBuffer to the encoder (takes the reference); capture_us is
	// when the frame was captured (steady clock, microseconds)
	void SubmitVideoBuffer(void* pixel_buffer, uint64_t capture_us);
	// gives back a buffer that won't be submitted
	void ReleaseVideoBuffer(void* pixel_buffer);
	// the buffer's IOSurfaceRef (for a Metal texture), owned by the buffer
	void* GetIOSurface(void* pixel_buffer);
	uint64_t NowUs();                          // the clock capture_us uses

	// camera
	bool PhoneCameraSelected();               // camera source says phone, and a phone with a camera is connected
	void SetCameraWanted(bool wanted);        // a game has the camera open
	// newest phone frame, centre-cropped/scaled to width x height NV12 (Y plane, then interleaved UV)
	bool GetCameraFrameNV12(uint8_t* dst, uint32_t width, uint32_t height, uint32_t pitch);

	// microphone
	bool PhoneMicSelected();
	void SetMicWanted(bool wanted);           // a game has the GamePad mic open
	// count samples at 32 kHz; false if the phone mic isn't the source (use the host device)
	bool ReadMic(int16_t* dst, size_t count);
}
