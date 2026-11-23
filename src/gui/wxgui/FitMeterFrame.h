#pragma once

#include <wx/frame.h>
#include <wx/timer.h>

class wxButton;
class wxCheckBox;
class wxSpinCtrl;
class wxStaticText;

// Tools > Virtual Fit Meter: the virtual Wii Fit Meter behind the GamePad's IR port (see FIT_METER.md)
class FitMeterFrame : public wxFrame
{
public:
	FitMeterFrame(wxWindow* parent);

private:
	void Refresh(bool force = false);
	void OnTimer(wxTimerEvent& event);

	wxTimer m_timer;
	wxCheckBox* m_enabled;
	wxCheckBox* m_autoPress;
	wxButton* m_press;
	wxStaticText* m_link;
	wxStaticText* m_registration;
	wxStaticText* m_clock;
	wxStaticText* m_lastSync;
	wxStaticText* m_activity;
	wxStaticText* m_folder;
	wxSpinCtrl* m_stepsToday;
	std::string m_lastStatusKey;
};
