#pragma once

#include "input/api/ControllerProvider.h"

// fork: the Mouse device (see MouseInputs.h, input/MouseState.h)
class MouseControllerProvider : public ControllerProviderBase
{
	friend class MouseController;
public:
	inline static InputAPI::Type kAPIType = InputAPI::Mouse;
	InputAPI::Type api() const override { return kAPIType; }

	std::vector<std::shared_ptr<ControllerBase>> get_controllers() override;
};
