#pragma once

#include "input/emulated/ProController.h"
#include "wxgui/input/panels/InputPanel.h"

class wxInputDraw;

class ProControllerInputPanel : public InputPanel
{
public:
	ProControllerInputPanel(wxWindow* parent);

	void on_timer(const EmulatedControllerPtr& emulated_controller, const ControllerPtr& controller) override;

private:
	wxInputDraw* m_left_draw, * m_right_draw;
};
