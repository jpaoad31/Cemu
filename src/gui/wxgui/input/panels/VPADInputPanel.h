#pragma once

#include "wxgui/input/panels/InputPanel.h"

class wxInputDraw;
class wxCheckBox;
class wxValueBars;
class wxTouchDraw;

class VPADInputPanel : public InputPanel
{
public:
	VPADInputPanel(wxWindow* parent);

	void on_timer(const EmulatedControllerPtr& emulated_controller, const ControllerPtr& controller) override;
	void load_controller(const EmulatedControllerPtr& controller) override;

private:
	wxInputDraw* m_left_draw, * m_right_draw;
	wxCheckBox* m_togglePadViewCheckBox;
	wxValueBars* m_motion;
	wxTouchDraw* m_touch;
};
