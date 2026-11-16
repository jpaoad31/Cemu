#pragma once

#include <wx/dialog.h>
#include <wx/notebook.h>
#include <wx/timer.h>

#include "input/api/InputAPI.h"

#include <boost/signals2/connection.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

struct ControllerPage;
class ControllerBase;
class InputDetector;
class wxStaticText;

// Options > Input settings (reworked in this fork, see INPUT_REWORK.md): one tab per emulated
// controller; each has one device (a dropdown with its state, as in Dolphin), the device's
// buttons, warnings, and the emulated controller's binding fields with live state.
class InputSettings2 : public wxDialog
{
public:
	InputSettings2(wxWindow* parent);
	~InputSettings2();

	// a device the dropdown offers: an instance from its API's provider, or one created when picked
	// (Keyboard/Mouse; the Remote GamePad, so listing it doesn't start listening on its port)
	struct DeviceEntry
	{
		InputAPI::Type api;
		std::string uuid;
		std::string name;
		std::shared_ptr<ControllerBase> instance;
	};

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

	// rebuilds the current page: the device dropdown, Remote GamePad options, the binding panel
	void update_state();
	void rebuild_device_choice(ControllerPage& page_data);
	// the devices the dropdowns offer, listed by a background thread (providers can be slow)
	void refresh_devices();
	std::vector<DeviceEntry> m_available;
	bool m_refreshing = false;
	std::shared_ptr<std::atomic_bool> m_alive = std::make_shared<std::atomic_bool>(true);
	// selects a device for the current page's controller (asks about its default bindings)
	void select_device(std::shared_ptr<ControllerBase> device);
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

	void on_device_selected(wxCommandEvent& event);
	void on_device_other(wxCommandEvent& event);
	void on_clear_bindings(wxCommandEvent& event);
	void on_load_preset(wxCommandEvent& event);

	// clicks and the wheel anywhere while the window is open go to the Keyboard/Mouse device (Detect, live
	// fields: the game windows get no events while this modal window is open)
	std::unique_ptr<class wxEventFilter> m_mouse_filter;
	void update_settings_mouse();

	// the selected device
	void device_defaults();
	void device_settings(const std::shared_ptr<ControllerBase>& controller);
};
