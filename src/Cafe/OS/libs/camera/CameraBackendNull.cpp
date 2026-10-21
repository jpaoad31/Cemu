#include "Common/precompiled.h"
#include "CameraBackend.h"

// platforms without a host camera backend: the game gets its own buffers back
// unchanged, as before
namespace camera
{
	std::unique_ptr<CameraBackend> CreateCameraBackend()
	{
		return nullptr;
	}
}
