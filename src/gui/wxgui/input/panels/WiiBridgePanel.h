#pragma once

// The Wii BT Bridge settings and status (wii_bridge in the config): "Wii BT Bridge at <address>",
// Scan / Stop and a status line. The settings are global (one bridge serves Wii Remotes and the
// Balance Board alike); the panel is shown on both the Wii Remote and the Balance Board input
// panels. Only built with SUPPORTS_WIIMOTE.

#include <wx/panel.h>

class wxCheckBox;
class wxTextCtrl;
class wxButton;
class wxStaticText;
class wxTimer;
class wxTimerEvent;
class wxCommandEvent;

class WiiBridgePanel : public wxPanel
{
public:
	explicit WiiBridgePanel(wxWindow* parent);
	~WiiBridgePanel() override;

private:
	void on_settings_changed(wxCommandEvent& event);
	void on_scan(wxCommandEvent& event);
	void on_stop(wxCommandEvent& event);
	void on_timer(wxTimerEvent& event);

	wxCheckBox* m_enabled = nullptr;
	wxTextCtrl* m_address = nullptr;
	wxButton* m_scan = nullptr;
	wxButton* m_stop = nullptr;
	wxStaticText* m_status = nullptr;
	wxTimer* m_timer = nullptr;
};
