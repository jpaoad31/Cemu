#pragma once

#include <wx/dialog.h>
#include <wx/notebook.h>
#include <wx/timer.h>

#include "input/api/InputAPI.h"

#include <boost/signals2/connection.hpp>

struct ControllerPage;
class ControllerBase;
class InputDetector;
class wxStaticText;

// Options > Input settings (reworked in this fork, see INPUT_REWORK.md): one tab per emulated
// controller; each shows its devices with their state and per-device buttons, warnings about
// conflicting assignments, and the emulated controller's binding fields with live state.
class InputSettings2 : public wxDialog
{
public:
	InputSettings2(wxWindow* parent);
	~InputSettings2();

private:
	void grow_to_fit();
	void restore_geometry();
	void save_geometry();
	const wxString kDefaultProfileName = _("<profile name>");

	wxNotebook* m_notebook;
	wxTimer* m_timer;
	wxStaticText* m_status_line;
	std::unique_ptr<InputDetector> m_detector;

	wxWindow* initialize_page(size_t index);
	ControllerPage& get_current_page_data() const;

	// rebuilds the current page: device rows, Remote GamePad options, the binding panel
	void update_state();
	void rebuild_device_rows(ControllerPage& page_data);
	void update_device_status(bool force = false);
	void update_remote_gamepad_status();
	void set_status(const wxString& text, bool error = false);
	uint32 m_tick = 0;

	boost::signals2::connection m_controller_changed;
	void on_controller_changed();

	// events
	void on_timer(wxTimerEvent& event);
	void on_char_hook(wxKeyEvent& event);
	void on_controller_page_changed(wxBookCtrlEvent& event);

	void on_profile_dropdown(wxCommandEvent& event);
	void on_profile_text_changed(wxCommandEvent& event);
	void on_profile_load(wxCommandEvent& event);
	void on_profile_save(wxCommandEvent& event);
	void on_profile_delete(wxCommandEvent& event);

	void on_emulated_controller_selected(wxCommandEvent& event);
	void on_emulated_controller_dropdown(wxCommandEvent& event);

	void on_controller_add(wxCommandEvent& event);
	void on_clear_bindings(wxCommandEvent& event);

	// per device
	void device_defaults(const std::shared_ptr<ControllerBase>& controller);
	void device_settings(const std::shared_ptr<ControllerBase>& controller);
	void device_remove(const std::shared_ptr<ControllerBase>& controller);
};
