#pragma once

#include <wx/panel.h>
#include <wx/settings.h>

#include "input/emulated/EmulatedController.h"
#include "input/api/Controller.h"
#include "wxgui/input/MappingControl.h"

#include "wxgui/wxHelper.h"

class ControllerBase;
class wxStaticBoxSizer;
class wxFlexGridSizer;

// Base of the emulated controller panels (fork rework, see INPUT_REWORK.md): binding fields
// (MappingControl) with live state, detect / clear / advanced per field.
class InputPanel : public wxPanel
{
public:
	InputPanel(wxWindow* parent);
	~InputPanel() override;

	// live state; called by the settings window's timer after it refreshed the devices
	virtual void on_timer(const EmulatedControllerPtr& emulated_controller, const ControllerPtr& controller);

	void reset_configuration();
	virtual void load_controller(const EmulatedControllerPtr& controller);
	// binding texts from the controller (after a change)
	void refresh_bindings();

	// the settings window shows these (detect results) and refreshes its device list
	void set_status_handler(std::function<void(const wxString&, bool error)> handler) { m_status_handler = std::move(handler); }
	void set_devices_changed_handler(std::function<void()> handler) { m_devices_changed_handler = std::move(handler); }

protected:
	MappingControl* create_mapping_control(wxWindow* parent, uint64 mapping, const wxString& label = {}, bool analogue = false);
	// a titled group with a "label: field" row per control; returns the box sizer (its static box
	// is the parent of the rows' windows)
	wxStaticBoxSizer* create_group(wxWindow* parent, const wxString& title, const std::vector<std::pair<uint64, wxString>>& rows, bool analogue = false, wxFlexGridSizer** grid_out = nullptr);

	EmulatedControllerPtr m_controller;
	std::vector<MappingControl*> m_controls;
	std::unordered_map<uint64, wxString> m_labels;

private:
	void on_mapping_action(MappingControl* control, MappingControl::Action action);
	void show_menu(MappingControl* control);
	void open_advanced(MappingControl* control);
	void report(const wxString& text, bool error = false);

	std::function<void(const wxString&, bool)> m_status_handler;
	std::function<void()> m_devices_changed_handler;
};
