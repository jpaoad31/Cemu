#pragma once

#include "input/emulated/ClassicController.h"
#include "wxgui/input/panels/InputPanel.h"

class wxInputDraw;

class ClassicControllerInputPanel : public InputPanel
{
public:
	ClassicControllerInputPanel(wxWindow* parent);

	void on_timer(const EmulatedControllerPtr& emulated_controller, const ControllerPtr& controller) override;

private:
	wxInputDraw* m_left_draw, * m_right_draw;
};
