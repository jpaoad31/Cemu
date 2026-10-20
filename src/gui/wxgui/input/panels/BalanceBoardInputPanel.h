#pragma once

#include "wxgui/input/panels/InputPanel.h"
#include "input/emulated/BalanceBoardController.h"

class wxGridBagSizer;
class wxInputDraw;
class wxSlider;
class wxStaticText;

class BalanceBoardInputPanel : public InputPanel
{
public:
	BalanceBoardInputPanel(wxWindow* parent);

	void on_timer(const EmulatedControllerPtr& emulated_controller, const ControllerPtr& controller) override;
	void load_controller(const EmulatedControllerPtr& emulated_controller) override;

private:
	void add_button_row(sint32 row, sint32 column, BalanceBoardController::ButtonId button_id);
	wxSlider* add_setting_row(sint32 row, sint32 column, const wxString& name, int min, int max, wxStaticText*& value_text);

	void on_setting_changed(wxCommandEvent& event);
	void update_setting_texts();

	wxGridBagSizer* m_item_sizer;
	wxInputDraw* m_draw;

	wxSlider* m_weight;
	wxSlider* m_foot_separation;
	wxSlider* m_foot_length;
	wxSlider* m_battery;
	wxStaticText* m_weight_text;
	wxStaticText* m_foot_separation_text;
	wxStaticText* m_foot_length_text;
	wxStaticText* m_battery_text;

	wxStaticText* m_status;

	std::weak_ptr<BalanceBoardController> m_board;
};
