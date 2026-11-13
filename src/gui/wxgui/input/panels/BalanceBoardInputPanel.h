#pragma once

#include "wxgui/input/panels/InputPanel.h"
#include "input/emulated/BalanceBoardController.h"

class wxGridBagSizer;
class wxInputDraw;
class wxSlider;
class wxStaticText;
class wxButton;
class wxTimer;
class wxTimerEvent;
class wxCheckBox;
class wxTextCtrl;

class BalanceBoardInputPanel : public InputPanel
{
public:
	BalanceBoardInputPanel(wxWindow* parent);
	~BalanceBoardInputPanel() override;

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

#ifdef HAS_IOBLUETOOTH
	// macOS: connect a real board over IOBluetooth (no pairing)
	void on_bt_connect(wxCommandEvent& event);
	void on_bt_cancel(wxCommandEvent& event);
	void on_bt_timer(wxTimerEvent& event);
	wxButton* m_bt_connect = nullptr;
	wxButton* m_bt_cancel = nullptr;
	wxStaticText* m_bt_status = nullptr;
	wxTimer* m_bt_timer = nullptr;
#endif

#ifdef SUPPORTS_WIIMOTE
	// Wii BT Bridge (UDP): a Pico W / Pi that connects real Wii devices for Cemu
	void on_bridge_settings_changed(wxCommandEvent& event);
	void on_bridge_scan(wxCommandEvent& event);
	void on_bridge_stop(wxCommandEvent& event);
	void on_bridge_timer(wxTimerEvent& event);
	wxCheckBox* m_bridge_enabled = nullptr;
	wxTextCtrl* m_bridge_address = nullptr;
	wxButton* m_bridge_scan = nullptr;
	wxButton* m_bridge_stop = nullptr;
	wxStaticText* m_bridge_status = nullptr;
	wxTimer* m_bridge_timer = nullptr;
#endif

	std::weak_ptr<BalanceBoardController> m_board;
};
