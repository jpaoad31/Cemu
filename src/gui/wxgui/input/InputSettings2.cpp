#include "wxgui/input/InputSettings2.h"

#include "input/InputManager.h"
#include "input/InputDiagnostics.h"
#include "wxgui/helpers/wxHelpers.h"
#include "wxgui/helpers/wxCustomData.h"

#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/notebook.h>
#include <wx/wupdlock.h>
#include <wx/stattext.h>
#include <wx/choice.h>
#include <wx/combobox.h>
#include <wx/button.h>
#include <wx/statline.h>
#include <wx/settings.h>
#include <wx/scrolwin.h>
#include <wx/display.h>
#include <wx/msgdlg.h>
#include <wx/checkbox.h>
#include <wx/app.h>

#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "wxgui/wxCemuConfig.h"
#include "wxgui/input/InputAPIAddWindow.h"
#include "wxgui/input/InputDetector.h"
#include "input/ControllerFactory.h"
#include "input/InputPresets.h"
#include "input/MouseState.h"
#include "wxgui/input/MouseFeed.h"
#include <wx/menu.h>
#include <wx/eventfilter.h>
#include <wx/utils.h>

#ifdef HAS_BLUEZ
#include "wxgui/input/PairingDialog.h"
#endif

#include "wxgui/input/panels/VPADInputPanel.h"
#include "wxgui/input/panels/ProControllerInputPanel.h"
#include "wxgui/input/panels/ClassicControllerInputPanel.h"
#include "wxgui/input/panels/WiimoteInputPanel.h"
#include "wxgui/input/panels/BalanceBoardInputPanel.h"
#include "wxgui/input/settings/DefaultControllerSettings.h"
#include "wxgui/input/settings/WiimoteControllerSettings.h"
#include "util/EventService.h"
#include "input/api/RemoteGamePad/RemoteGamePadController.h"
#include "input/api/RemoteGamePad/RemoteGamePadMedia.h"

bool g_inputConfigWindowHasFocus = false;

struct ControllerPage
{
	size_t index = 0;
	wxScrolledWindow* page = nullptr;
	EmulatedControllerPtr m_controller;

	// profiles
	wxComboBox* m_profiles;
	wxButton* m_profile_load, * m_profile_save, * m_profile_delete;
	wxStaticText* m_profile_status;

	// emulated controller
	wxComboBox* m_emulated_controller;

	// the device (one per emulated controller)
	wxStaticBoxSizer* m_devices_box = nullptr;
	wxChoice* m_device_choice = nullptr;
	std::vector<InputSettings2::DeviceEntry> m_choice_entries; // item i + 1 (item 0: none)
	wxButton* m_refresh = nullptr;
	wxButton* m_other = nullptr;
	wxCheckBox* m_all_devices = nullptr;
	wxStaticText* m_device_status = nullptr;
	wxButton* m_defaults = nullptr;
	wxButton* m_settings = nullptr;
	wxButton* m_calibrate = nullptr;
	wxButton* m_clear = nullptr;
	wxButton* m_preset = nullptr;
	wxStaticText* m_warnings = nullptr;
	wxStaticText* m_note = nullptr; // the migration note of an old profile (shown until the window closes)

	// Remote GamePad (shown while one is attached)
	wxStaticText* m_remote_status = nullptr; // port, phone, packet rate
	wxChoice* m_remote_camera = nullptr;     // GamePad camera source
	wxChoice* m_remote_mic = nullptr;        // GamePad mic source
	wxChoice* m_remote_speaker = nullptr;    // GamePad speaker target
	wxChoice* m_remote_video = nullptr;      // size of the video sent to the phone

	// binding panels, one per emulated type (created when first shown)
	wxBoxSizer* m_panel_sizer = nullptr;
	std::array<InputPanel*, EmulatedController::Type::MAX> m_panels{};
};
using wxControllerPageData = wxCustomData<ControllerPage>;

InputSettings2::InputSettings2(wxWindow* parent)
	: wxDialog(parent, wxID_ANY, _("Input settings"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
	g_inputConfigWindowHasFocus = true;
	m_detector = std::make_unique<InputDetector>();

	auto* sizer = new wxBoxSizer(wxVERTICAL);

	m_notebook = new wxNotebook(this, wxID_ANY);
	for (size_t i = 0; i < InputManager::kMaxController; ++i)
	{
		auto* page = new wxScrolledWindow(m_notebook, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxVSCROLL | wxHSCROLL);
		page->SetScrollRate(FromDIP(10), FromDIP(10));
		page->SetClientObject(nullptr); // force internal type to client object
		m_notebook->AddPage(page, formatWxString(_("Controller {}"), i + 1));
	}
	m_notebook->Bind(wxEVT_NOTEBOOK_PAGE_CHANGED, &InputSettings2::on_controller_page_changed, this);
	sizer->Add(m_notebook, 1, wxEXPAND);

	m_status_line = new wxStaticText(this, wxID_ANY, _("Click a binding and press an input on the controller's device to bind it. Shift-click adds an input; right click for more."));
	sizer->Add(m_status_line, 0, wxALL | wxEXPAND, FromDIP(6));

	m_notebook->SetSelection(0);
	initialize_page(0);

	SetSizer(sizer);
	update_state();
	Layout();
	Fit();
	grow_to_fit(); // a scrolled page reports a small best size: fit its content
	restore_geometry();

	Bind(wxEVT_CHAR_HOOK, &InputSettings2::on_char_hook, this);
	Bind(wxEVT_TIMER, &InputSettings2::on_timer, this);
	m_timer = new wxTimer(this);
	m_timer->Start(25);

	m_controller_changed = EventService::instance().connect<Events::ControllerChanged>(&InputSettings2::on_controller_changed, this);
	refresh_devices();

	// the wheel anywhere in this window (and its dialogs) goes to the Keyboard/Mouse device while it's open
	struct WheelFilter : wxEventFilter
	{
		int FilterEvent(wxEvent& event) override
		{
			if (event.GetEventType() == wxEVT_MOUSEWHEEL)
			{
				auto& wheel = static_cast<wxMouseEvent&>(event);
				if (wheel.GetWheelAxis() == wxMOUSE_WHEEL_VERTICAL && wheel.GetWheelDelta() > 0)
					MouseState::Get().on_wheel(MouseState::kSettings, (float)wheel.GetWheelRotation() / (float)wheel.GetWheelDelta());
			}
			return Event_Skip;
		}
	};
	m_mouse_filter = std::make_unique<WheelFilter>();
	wxEvtHandler::AddFilter(m_mouse_filter.get());

	if (const char* page = getenv("CEMU_TEST_INPUT_SETTINGS_PAGE")) // test hook, see MainWindow
		m_notebook->SetSelection(std::clamp(atoi(page), 0, (int)InputManager::kMaxController - 1));
}

InputSettings2::~InputSettings2()
{
	*m_alive = false;
	wxEvtHandler::RemoveFilter(m_mouse_filter.get());
	MouseState::Get().set_settings_buttons(0, false);
	m_controller_changed.disconnect();
	m_detector->cancel();

	g_inputConfigWindowHasFocus = false;
	m_timer->Stop();
	save_geometry();
	InputManager::instance().save();
}

void InputSettings2::restore_geometry()
{
	const auto& config = GetWxGUIConfig();
	wxSize size = GetSize();
	if (config.input_window_size.x > 0 && config.input_window_size.y > 0)
		size = wxSize(config.input_window_size.x, config.input_window_size.y);
	size.IncTo(GetMinSize());

	if (config.input_window_position.x != -1 || config.input_window_position.y != -1)
	{
		const wxPoint pos(config.input_window_position.x, config.input_window_position.y);
		// only where it is still on a screen
		if (wxDisplay::GetFromPoint(pos + wxPoint(40, 20)) != wxNOT_FOUND)
		{
			SetSize(wxRect(pos, size));
			return;
		}
	}
	SetSize(size);
	CentreOnParent();
}

void InputSettings2::save_geometry()
{
	auto& config = GetWxGUIConfig();
	const wxRect r = GetRect();
	config.input_window_position = {r.x, r.y};
	config.input_window_size = {r.width, r.height};
	GetConfigHandle().Save();
}

wxWindow* InputSettings2::initialize_page(size_t index)
{
	auto* page = (wxScrolledWindow*)m_notebook->GetPage(index);
	if (page->GetClientObject()) // already initialized
		return page;

	ControllerPage page_data{};
	page_data.index = index;
	page_data.page = page;
	const auto emulated_controller = InputManager::instance().get_controller(index);
	page_data.m_controller = emulated_controller;

	wxWindowUpdateLocker lock(page);
	auto* sizer = new wxBoxSizer(wxVERTICAL);
	const int gap = FromDIP(5);

	{
		// profile
		auto* row = new wxBoxSizer(wxHORIZONTAL);
		row->Add(new wxStaticText(page, wxID_ANY, _("Profile")), 0, wxALIGN_CENTER_VERTICAL | wxALL, gap);
		auto* profiles = new wxComboBox(page, wxID_ANY, kDefaultProfileName);
		profiles->SetMinSize(FromDIP(wxSize(200, -1)));
		row->Add(profiles, 1, wxALIGN_CENTER_VERTICAL | wxALL, gap);

#if BOOST_OS_LINUX || BOOST_OS_BSD
		// We rely on the wxEVT_COMBOBOX_DROPDOWN event to trigger filling the profile list,
		// but on wxGTK the dropdown button cannot be clicked if the list is empty
		// so as a quick and dirty workaround we fill the list here
		wxCommandEvent tmpCmdEvt;
		tmpCmdEvt.SetEventObject(profiles);
		on_profile_dropdown(tmpCmdEvt);
#endif
		if (emulated_controller && emulated_controller->has_profile_name())
			profiles->SetValue(emulated_controller->get_profile_name());

		auto* load_bttn = new wxButton(page, wxID_ANY, _("Load"));
		load_bttn->Disable();
		row->Add(load_bttn, 0, wxALIGN_CENTER_VERTICAL | wxALL, gap);
		auto* save_bttn = new wxButton(page, wxID_ANY, _("Save"));
		save_bttn->Disable();
		row->Add(save_bttn, 0, wxALIGN_CENTER_VERTICAL | wxALL, gap);
		auto* delete_bttn = new wxButton(page, wxID_ANY, _("Delete"));
		delete_bttn->Disable();
		row->Add(delete_bttn, 0, wxALIGN_CENTER_VERTICAL | wxALL, gap);

		auto* profile_status = new wxStaticText(page, wxID_ANY, _("controller set by gameprofile. changes won't be saved permanently!"));
		profile_status->SetMinSize(FromDIP(wxSize(200, -1)));
		profile_status->Wrap(FromDIP(200));
		row->Add(profile_status, 1, wxALIGN_CENTER_VERTICAL | wxALL | wxRESERVE_SPACE_EVEN_IF_HIDDEN, gap);
		if (InputManager::instance().is_gameprofile_set(index))
			profile_status->SetForegroundColour(wxTheColourDatabase->Find("ERROR"));
		else
			profile_status->Hide();

		load_bttn->Bind(wxEVT_BUTTON, &InputSettings2::on_profile_load, this);
		save_bttn->Bind(wxEVT_BUTTON, &InputSettings2::on_profile_save, this);
		delete_bttn->Bind(wxEVT_BUTTON, &InputSettings2::on_profile_delete, this);
		profiles->Bind(wxEVT_COMBOBOX_DROPDOWN, &InputSettings2::on_profile_dropdown, this);
		profiles->Bind(wxEVT_TEXT, &InputSettings2::on_profile_text_changed, this);

		page_data.m_profiles = profiles;
		page_data.m_profile_load = load_bttn;
		page_data.m_profile_save = save_bttn;
		page_data.m_profile_delete = delete_bttn;
		page_data.m_profile_status = profile_status;
		sizer->Add(row, 0, wxEXPAND);
	}

	{
		// emulated controller
		auto* row = new wxBoxSizer(wxHORIZONTAL);
		row->Add(new wxStaticText(page, wxID_ANY, _("Emulated controller")), 0, wxALIGN_CENTER_VERTICAL | wxALL, gap);
		auto* econtroller_box = new wxComboBox(page, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_READONLY);
		econtroller_box->SetMinSize(FromDIP(wxSize(220, -1)));
		econtroller_box->Bind(wxEVT_COMBOBOX_DROPDOWN, &InputSettings2::on_emulated_controller_dropdown, this);
		econtroller_box->Bind(wxEVT_COMBOBOX, &InputSettings2::on_emulated_controller_selected, this);
		econtroller_box->AppendString(_("Disabled"));
		econtroller_box->SetSelection(0);
		row->Add(econtroller_box, 0, wxALIGN_CENTER_VERTICAL | wxALL, gap);
		page_data.m_emulated_controller = econtroller_box;
		sizer->Add(row, 0, wxEXPAND);
	}

	{
		// the device: plain bindings are its inputs, Detect listens to it (Dolphin's model)
		auto* box = new wxStaticBoxSizer(wxVERTICAL, page, _("Device"));
		page_data.m_devices_box = box;
		auto* parent0 = box->GetStaticBox();

		auto* row = new wxBoxSizer(wxHORIZONTAL);
		page_data.m_device_choice = new wxChoice(parent0, wxID_ANY);
		page_data.m_device_choice->SetMinSize(FromDIP(wxSize(300, -1)));
		page_data.m_device_choice->SetToolTip(_("This controller's device. Bindings without a device name are its inputs, and its own data (motion, touch, the Wii Remote's pointer, a real board) is used. Another device's input can still be named explicitly in a binding's Advanced... editor."));
		page_data.m_device_choice->Bind(wxEVT_CHOICE, &InputSettings2::on_device_selected, this);
		row->Add(page_data.m_device_choice, 1, wxALIGN_CENTER_VERTICAL | wxALL, gap);
		page_data.m_refresh = new wxButton(parent0, wxID_ANY, _("Refresh"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
		page_data.m_refresh->SetToolTip(_("Look for connected devices again."));
		page_data.m_refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { refresh_devices(); });
		row->Add(page_data.m_refresh, 0, wxALIGN_CENTER_VERTICAL | wxALL, gap);
		page_data.m_other = new wxButton(parent0, wxID_ANY, _("Other device..."), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
		page_data.m_other->SetToolTip(_("A device by API: a DSU client (IP and port), a Remote GamePad on another port, ..."));
		page_data.m_other->Bind(wxEVT_BUTTON, &InputSettings2::on_device_other, this);
		row->Add(page_data.m_other, 0, wxALIGN_CENTER_VERTICAL | wxALL, gap);
		page_data.m_all_devices = new wxCheckBox(parent0, wxID_ANY, _("Detect on all devices"));
		page_data.m_all_devices->SetToolTip(_("Detect listens to every device, not only this controller's. An input of another device is written with its device name (an explicit, advanced binding)."));
		page_data.m_all_devices->SetValue(m_detector->all_devices());
		page_data.m_all_devices->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent& e) { m_detector->cancel(); m_detector->set_all_devices(e.IsChecked()); });
		row->Add(page_data.m_all_devices, 0, wxALIGN_CENTER_VERTICAL | wxALL, gap);
		box->Add(row, 0, wxEXPAND);

		page_data.m_device_status = new wxStaticText(parent0, wxID_ANY, wxEmptyString);
		box->Add(page_data.m_device_status, 0, wxLEFT | wxRIGHT | wxEXPAND, gap * 2);

		auto* buttons = new wxBoxSizer(wxHORIZONTAL);
		page_data.m_defaults = new wxButton(parent0, wxID_ANY, _("Default bindings"));
		page_data.m_defaults->SetToolTip(_("Replace all bindings of this controller with the device's defaults."));
		page_data.m_defaults->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { device_defaults(); });
		buttons->Add(page_data.m_defaults, 0, wxALL, gap);
		page_data.m_settings = new wxButton(parent0, wxID_ANY, _("Settings..."));
		page_data.m_settings->SetToolTip(_("Dead zones, ranges, rumble, motion; Wii Remote options (speaker, ...)."));
		page_data.m_settings->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			auto& data = get_current_page_data();
			if (const auto device = data.m_controller ? data.m_controller->get_device() : nullptr)
				device_settings(device);
		});
		buttons->Add(page_data.m_settings, 0, wxALL, gap);
		page_data.m_calibrate = new wxButton(parent0, wxID_ANY, _("Calibrate"));
		page_data.m_calibrate->SetToolTip(_("Take the current stick and trigger positions as the rest position."));
		page_data.m_calibrate->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			auto& data = get_current_page_data();
			if (const auto device = data.m_controller ? data.m_controller->get_device() : nullptr)
			{
				device->calibrate();
				set_status(wxString::Format(_("%s calibrated."), wxString::FromUTF8(device->display_name())));
			}
		});
		buttons->Add(page_data.m_calibrate, 0, wxALL, gap);
		page_data.m_clear = new wxButton(parent0, wxID_ANY, _("Clear all bindings"));
		page_data.m_clear->Bind(wxEVT_BUTTON, &InputSettings2::on_clear_bindings, this);
		buttons->Add(page_data.m_clear, 0, wxALL, gap);
		page_data.m_preset = new wxButton(parent0, wxID_ANY, _("Load preset..."));
		page_data.m_preset->SetToolTip(_("A device and its bindings in one step (Keyboard/Mouse, Remote GamePad, a gamepad, a real Wii Remote or board). Everything shows in the fields below and can be edited after."));
		page_data.m_preset->Bind(wxEVT_BUTTON, &InputSettings2::on_load_preset, this);
		buttons->Add(page_data.m_preset, 0, wxALL, gap);
#ifdef HAS_BLUEZ
		auto* pairing = new wxButton(parent0, wxID_ANY, _("Pair Wii/Wii U Controller"));
		pairing->Bind(wxEVT_BUTTON, [this](wxEvent&) {
			PairingDialog pairing_dialog(this);
			pairing_dialog.ShowModal();
		});
		buttons->Add(pairing, 0, wxALL, gap);
#endif
		box->Add(buttons, 0);

		// Remote GamePad options
		auto* parent = box->GetStaticBox();
		auto* remote_sizer = new wxBoxSizer(wxVERTICAL);
		page_data.m_remote_status = new wxStaticText(parent, wxID_ANY, wxEmptyString);
		page_data.m_remote_status->Hide();
		remote_sizer->Add(page_data.m_remote_status, 0, wxALL, 0);
		auto* remote_choices = new wxBoxSizer(wxHORIZONTAL);
		auto* remote_camera = new wxChoice(parent, wxID_ANY);
		remote_camera->Append(_("GamePad camera: phone when connected, else Mac"));
		remote_camera->Append(_("GamePad camera: Mac camera"));
		remote_camera->SetSelection((int)RemoteGamePad::Media::GetCameraSource());
		remote_camera->Bind(wxEVT_CHOICE, [](wxCommandEvent& e) { RemoteGamePad::Media::SetCameraSource((RemoteGamePad::Media::CameraSource)e.GetSelection()); });
		remote_camera->Hide();
		auto* remote_mic = new wxChoice(parent, wxID_ANY);
		remote_mic->Append(_("GamePad mic: phone when connected"));
		remote_mic->Append(_("GamePad mic: Cemu's audio input device"));
		remote_mic->SetSelection((int)RemoteGamePad::Media::GetMicSource());
		remote_mic->Bind(wxEVT_CHOICE, [](wxCommandEvent& e) { RemoteGamePad::Media::SetMicSource((RemoteGamePad::Media::MicSource)e.GetSelection()); });
		remote_mic->Hide();
		auto* remote_speaker = new wxChoice(parent, wxID_ANY);
		remote_speaker->Append(_("GamePad audio: phone when connected, else Mac"));
		remote_speaker->Append(_("GamePad audio: Mac (GamePad audio device)"));
		remote_speaker->Append(_("GamePad audio: phone and Mac"));
		remote_speaker->SetSelection((int)RemoteGamePad::Media::GetSpeakerTarget());
		remote_speaker->Bind(wxEVT_CHOICE, [](wxCommandEvent& e) { RemoteGamePad::Media::SetSpeakerTarget((RemoteGamePad::Media::SpeakerTarget)e.GetSelection()); });
		remote_speaker->Hide();
		remote_choices->Add(remote_camera, 0, wxTOP | wxRIGHT, gap);
		remote_choices->Add(remote_mic, 0, wxTOP | wxRIGHT, gap);
		remote_choices->Add(remote_speaker, 0, wxTOP, gap);
		remote_sizer->Add(remote_choices);
		auto* remote_video = new wxChoice(parent, wxID_ANY);
		remote_video->Append(_("GamePad video: 854x480 (GamePad native)"));
		remote_video->Append(_("GamePad video: source resolution (up to 1280x720)"));
		remote_video->Append(_("GamePad video: source resolution, up to 1920x1080 (1.5x, with a resolution pack)"));
		remote_video->Append(_("GamePad video: source resolution, up to 2560x1440 (2x, with a resolution pack)"));
		remote_video->SetToolTip(_("Size of the GamePad picture sent to the phone. \"Source\" is the size the game renders the GamePad image at, as Cemu holds it (a resolution graphic pack scales it), capped; larger sizes use a higher bitrate."));
		remote_video->SetSelection((int)RemoteGamePad::Media::GetVideoResolution());
		remote_video->Bind(wxEVT_CHOICE, [](wxCommandEvent& e) { RemoteGamePad::Media::SetVideoResolution((RemoteGamePad::Media::VideoResolution)e.GetSelection()); });
		remote_video->Hide();
		remote_sizer->Add(remote_video, 0, wxTOP, gap);
		box->Add(remote_sizer, 0, wxLEFT | wxRIGHT | wxBOTTOM, gap);
		page_data.m_remote_camera = remote_camera;
		page_data.m_remote_mic = remote_mic;
		page_data.m_remote_speaker = remote_speaker;
		page_data.m_remote_video = remote_video;

		page_data.m_warnings = new wxStaticText(parent, wxID_ANY, wxEmptyString);
		page_data.m_warnings->SetForegroundColour(wxTheColourDatabase->Find("ERROR"));
		page_data.m_warnings->Hide();
		box->Add(page_data.m_warnings, 0, wxALL | wxEXPAND, gap);

		// an old profile with several devices on this controller: what was dropped (once)
		page_data.m_note = new wxStaticText(parent, wxID_ANY, wxEmptyString);
		page_data.m_note->SetForegroundColour(wxTheColourDatabase->Find("ERROR"));
		page_data.m_note->Hide();
		box->Add(page_data.m_note, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, gap);

		sizer->Add(box, 0, wxEXPAND | wxALL, gap);
	}

	page_data.m_panel_sizer = new wxBoxSizer(wxVERTICAL);
	sizer->Add(page_data.m_panel_sizer, 1, wxEXPAND | wxALL, gap);

	page->SetSizer(sizer);
	page->SetClientObject(new wxCustomData(page_data));
	page->Layout();
	return page;
}

ControllerPage& InputSettings2::get_current_page_data() const
{
	auto* page = m_notebook->GetCurrentPage();
	auto* page_data_ptr = (wxControllerPageData*)page->GetClientObject();
	wxASSERT(page_data_ptr);
	return page_data_ptr->ref();
}

void InputSettings2::set_status(const wxString& text, bool error)
{
	m_status_line->SetLabel(text);
	m_status_line->SetForegroundColour(error ? wxTheColourDatabase->Find("ERROR") : wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
	m_status_line->Refresh();
}

namespace
{
	bool same_device(const InputSettings2::DeviceEntry& e, const ControllerBase& c)
	{
		return e.api == c.api() && e.uuid == c.uuid();
	}
}

void InputSettings2::rebuild_device_choice(ControllerPage& page_data)
{
	auto* choice = page_data.m_device_choice;
	const auto current = page_data.m_controller ? page_data.m_controller->get_device() : nullptr;

	// the devices found, the current one in its place (or first when it isn't found)
	std::vector<DeviceEntry> entries;
	bool found = false;
	for (const auto& e : m_available)
	{
		if (current && same_device(e, *current))
		{
			entries.push_back({current->api(), current->uuid(), current->display_name(), current});
			found = true;
		}
		else
			entries.push_back(e);
	}
	if (current && !found)
		entries.insert(entries.begin(), {current->api(), current->uuid(), current->display_name(), current});

	wxArrayString labels;
	labels.Add(_("None"));
	int selection = 0;
	for (size_t i = 0; i < entries.size(); ++i)
	{
		const auto& e = entries[i];
		std::string label = e.api == InputAPI::Keyboard ? e.name : fmt::format("{}  [{}]", e.name, InputAPI::to_string(e.api));
		if (current && same_device(e, *current))
		{
			selection = (int)i + 1;
			if (!found && !m_refreshing)
				label += "  (not found)";
		}
		labels.Add(wxString::FromUTF8(label));
	}
	page_data.m_choice_entries = std::move(entries);
	if (choice->GetStrings() != labels)
	{
		choice->Set(labels);
		page_data.page->Layout();
	}
	choice->SetSelection(selection);
	choice->Enable(page_data.m_controller != nullptr);
}

void InputSettings2::refresh_devices()
{
	if (m_refreshing)
		return;
	m_refreshing = true;

	// the providers, taken here (the list grows on this thread when a device of a new DSU host or
	// Remote GamePad port is created)
	std::vector<ControllerProviderPtr> providers;
	for (const auto& list : InputManager::instance().get_api_providers())
	{
		for (const auto& p : list)
		{
			if (p->api() != InputAPI::Keyboard && p->api() != InputAPI::RemoteGamePad)
				providers.push_back(p);
		}
	}
	std::thread([this, alive = m_alive, providers = std::move(providers)] {
		std::vector<DeviceEntry> list;
		list.push_back({InputAPI::Keyboard, "keyboard", "Keyboard/Mouse", nullptr});
#if HAS_REMOTE_GAMEPAD
		// listed without an instance: creating one starts listening on its port
		list.push_back({InputAPI::RemoteGamePad, std::to_string(RemoteGamePad::kDefaultPort), fmt::format("Remote GamePad (UDP {})", RemoteGamePad::kDefaultPort), nullptr});
#endif
		for (const auto& provider : providers)
		{
			try
			{
				for (const auto& c : provider->get_controllers())
					list.push_back({c->api(), c->uuid(), c->display_name(), c});
			}
			catch (const std::exception& ex)
			{
				cemuLog_log(LogType::InputAPI, "input settings: can't list {} devices: {}", provider->api_name(), ex.what());
			}
		}
		wxTheApp->CallAfter([this, alive, list = std::move(list)]() mutable {
			if (!*alive)
				return;
			m_available = std::move(list);
			m_refreshing = false;
			if (m_notebook->GetCurrentPage() && m_notebook->GetCurrentPage()->GetClientObject())
				rebuild_device_choice(get_current_page_data());
		});
	}).detach();
}

void InputSettings2::update_state()
{
	auto& page_data = get_current_page_data();
	auto* page = page_data.page;
	wxWindowUpdateLocker lock(page);

	const EmulatedControllerPtr emulated_controller = page_data.m_controller;

	// emulated controller box
	if (emulated_controller)
	{
		const auto name = wxString::FromUTF8(emulated_controller->type_to_string(emulated_controller->type()));
		if (page_data.m_emulated_controller->FindString(name) == wxNOT_FOUND)
			page_data.m_emulated_controller->Append(name);
		page_data.m_emulated_controller->SetStringSelection(name);
	}
	else
		page_data.m_emulated_controller->SetSelection(0);

	const auto device = emulated_controller ? emulated_controller->get_device() : nullptr;
	const bool pad = device && device->api() != InputAPI::Keyboard;
	page_data.m_other->Enable(emulated_controller != nullptr);
	page_data.m_defaults->Enable(device != nullptr);
	page_data.m_settings->Enable(pad);
	page_data.m_calibrate->Enable(pad);
	page_data.m_clear->Enable(emulated_controller != nullptr);
	page_data.m_preset->Enable(emulated_controller != nullptr);
	page_data.m_all_devices->SetValue(m_detector->all_devices());
	rebuild_device_choice(page_data);

	// an old profile's dropped devices (InputManager::load), shown until the window closes
	if (const auto note = InputManager::instance().take_migration_note(page_data.index); !note.empty())
	{
		page_data.m_note->SetLabel(wxString::FromUTF8(note));
		page_data.m_note->Wrap(FromDIP(700));
		page_data.m_note->Show();
	}

	// binding panel
	for (auto* panel : page_data.m_panels)
	{
		if (panel && (!emulated_controller || panel != page_data.m_panels[emulated_controller->type()]))
			panel->Hide();
	}
	if (emulated_controller)
	{
		const auto type = emulated_controller->type();
		InputPanel* panel = page_data.m_panels[type];
		if (!panel)
		{
			switch (type)
			{
			case EmulatedController::Type::VPAD: panel = new VPADInputPanel(page); break;
			case EmulatedController::Pro: panel = new ProControllerInputPanel(page); break;
			case EmulatedController::Classic: panel = new ClassicControllerInputPanel(page); break;
			case EmulatedController::Wiimote: panel = new WiimoteInputPanel(page); break;
			case EmulatedController::BalanceBoard: panel = new BalanceBoardInputPanel(page); break;
			default:
				cemu_assert_debug(false);
				return;
			}
			panel->set_status_handler([this](const wxString& text, bool error) { set_status(text, error); });
			panel->set_devices_changed_handler([this] {
				auto& data = get_current_page_data();
				rebuild_device_choice(data);
				update_device_status(true);
				data.page->Layout();
				data.page->FitInside();
			});
			page_data.m_panels[type] = panel;
			page_data.m_panel_sizer->Add(panel, 1, wxEXPAND);
		}
		// hidden panels of other types keep their old controller pointer but are reloaded here
		// before they're shown again (a panel only takes its own type)
		panel->load_controller(emulated_controller);
		panel->Show();
		panel->Layout();
		panel->InvalidateBestSize();
	}

	update_device_status(true);
	page->Layout();
	page->FitInside();
	grow_to_fit();
}

// Panels differ a lot in size (the Balance Board panel is much taller than the GamePad's): grow the
// window when the shown page needs more room, never past the screen and never shrinking it (the
// page scrolls when the window is smaller than its content).
void InputSettings2::grow_to_fit()
{
	if (!GetSizer())
		return;
	auto* page = (wxScrolledWindow*)m_notebook->GetCurrentPage();
	for (auto* child : page->GetChildren())
		child->InvalidateBestSize();
	const wxSize page_best = page->GetSizer() ? page->GetSizer()->CalcMin() : wxSize();
	const wxSize page_now = page->GetClientSize();
	wxSize size = GetSize();
	size.IncBy(std::max(0, page_best.x - page_now.x + FromDIP(4)), std::max(0, page_best.y - page_now.y + FromDIP(4)));
	const int display = wxDisplay::GetFromWindow(this);
	const wxRect area = wxDisplay(display == wxNOT_FOUND ? 0 : display).GetClientArea();
	size.DecTo(area.GetSize());
	if (size != GetSize())
	{
		SetSize(size);
		// keep it on the screen
		wxRect r = GetRect();
		if (r.GetRight() > area.GetRight())
			r.x = std::max(area.x, area.GetRight() - r.width);
		if (r.GetBottom() > area.GetBottom())
			r.y = std::max(area.y, area.GetBottom() - r.height);
		SetPosition(r.GetPosition());
		Layout();
	}
}

void InputSettings2::update_device_status(bool force)
{
	if (!force && m_tick % 10 != 0) // timer is 25 ms: refresh at ~4 Hz
		return;
	auto& page_data = get_current_page_data();

	bool relayout = false;
	{
		const auto device = page_data.m_controller ? page_data.m_controller->get_device() : nullptr;
		const bool connected = device && InputDiagnostics::IsDeviceConnected(*device);
		wxString text;
		if (!page_data.m_controller)
			text = _("Choose an emulated controller above.");
		else if (!device)
			text = _("No device: choose one above (Keyboard/Mouse, a gamepad, the Remote GamePad, a Wii device).");
		else
			text = wxString::FromUTF8(fmt::format("{}: {}", device->display_name(), InputDiagnostics::DeviceStatus(*device)));
		if (page_data.m_device_status->GetLabel() != text)
		{
			page_data.m_device_status->SetLabel(text);
			relayout = true;
		}
		const wxColour colour = connected ? wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT) : wxTheColourDatabase->Find("ERROR");
		if (page_data.m_device_status->GetForegroundColour() != colour)
		{
			page_data.m_device_status->SetForegroundColour(colour);
			page_data.m_device_status->Refresh();
		}
	}

	wxString warnings;
	if (page_data.m_controller)
	{
		for (const auto& w : InputDiagnostics::GetWarnings(page_data.index))
			warnings += (warnings.empty() ? wxString() : wxString("\n")) + wxString::FromUTF8((w.error ? "⚠ " : "Note: ") + w.text);
	}
	if (page_data.m_warnings->GetLabel() != warnings)
	{
		page_data.m_warnings->SetLabel(warnings);
		page_data.m_warnings->Wrap(FromDIP(700));
		relayout = true;
	}
	if (page_data.m_warnings->IsShown() == warnings.empty())
	{
		page_data.m_warnings->Show(!warnings.empty());
		relayout = true;
	}
	if (relayout)
	{
		page_data.page->Layout();
		page_data.page->FitInside();
		grow_to_fit();
	}
}

void InputSettings2::update_remote_gamepad_status()
{
	if (m_tick % 8 != 0) // timer is 25 ms: refresh at ~5 Hz
		return;
	auto& page_data = get_current_page_data();
	auto* text = page_data.m_remote_status;

	// the phone's options while the Remote GamePad is this controller's device
	std::shared_ptr<RemoteGamePadController> remote;
	if (page_data.m_controller)
		remote = std::dynamic_pointer_cast<RemoteGamePadController>(page_data.m_controller->get_device());
	const bool media = remote && RemoteGamePad::Media::IsSupported();
	bool relayout = false;
	for (wxChoice* c : {page_data.m_remote_camera, page_data.m_remote_mic, page_data.m_remote_speaker, page_data.m_remote_video})
	{
		if (c && c->IsShown() != media)
		{
			c->Show(media);
			if (media) // the setting may have come from a profile load
				c->SetSelection(c == page_data.m_remote_camera ? (int)RemoteGamePad::Media::GetCameraSource()
					: c == page_data.m_remote_mic ? (int)RemoteGamePad::Media::GetMicSource()
					: c == page_data.m_remote_video ? (int)RemoteGamePad::Media::GetVideoResolution()
					: (int)RemoteGamePad::Media::GetSpeakerTarget());
			relayout = true;
		}
	}
	if (!remote)
	{
		if (text->IsShown())
		{
			text->Hide();
			relayout = true;
		}
	}
	else
	{
		const auto label = wxString::FromUTF8(remote->get_status_text());
		if (text->GetLabel() != label)
			text->SetLabel(label);
		if (!text->IsShown())
		{
			text->Show();
			relayout = true;
		}
	}
	if (relayout)
	{
		page_data.page->Layout();
		page_data.page->FitInside();
		grow_to_fit();
	}
}

void InputSettings2::on_controller_changed()
{
	// devices came or went: refresh the state and the device list
	if (m_notebook->GetCurrentPage() && m_notebook->GetCurrentPage()->GetClientObject())
		update_device_status(true);
	refresh_devices();
}

void InputSettings2::on_char_hook(wxKeyEvent& event)
{
	if (m_detector->on_char_hook(event))
		return;
	event.Skip();
}

void InputSettings2::on_timer(wxTimerEvent&)
{
	++m_tick;
	update_settings_mouse();
	m_detector->poll();

	auto& page_data = get_current_page_data();
	update_remote_gamepad_status();
	update_device_status();
	if (!page_data.m_controller)
		return;

	// live state of the device (and the devices bindings name explicitly)
	for (const auto& c : page_data.m_controller->get_controllers())
		InputDetector::update_device(*c);

	if (auto* panel = page_data.m_panels[page_data.m_controller->type()])
		panel->on_timer(page_data.m_controller, nullptr);
}

void InputSettings2::on_controller_page_changed(wxBookCtrlEvent& event)
{
	m_detector->cancel();
	initialize_page(event.GetSelection());
	update_state();
	event.Skip();
}

void InputSettings2::on_emulated_controller_selected(wxCommandEvent& event)
{
	m_detector->cancel();
	const auto page_index = m_notebook->GetSelection();
	auto& page_data = get_current_page_data();

	const auto selection = event.GetSelection();
	if (selection == 0) // disabled selected
	{
		page_data.m_controller = {};
		InputManager::instance().delete_controller(page_index, true);
	}
	else
	{
		try
		{
			const auto type = EmulatedController::type_from_string(event.GetString().utf8_string());
			// same has already been selected
			if (page_data.m_controller && page_data.m_controller->type() == type)
				return;

			// the new emulated controller takes over the previous one's device (set_controller)
			// with that device's defaults for the new type
			const auto new_controller = InputManager::instance().set_controller(page_index, type);
			page_data.m_controller = new_controller;
			if (new_controller)
				new_controller->load_default_bindings();
		}
		catch (const std::exception&)
		{
			cemu_assert_debug(false);
		}
	}
	update_state();
}

void InputSettings2::on_device_selected(wxCommandEvent& event)
{
	auto& page_data = get_current_page_data();
	if (!page_data.m_controller)
		return;
	m_detector->cancel();
	const int selection = page_data.m_device_choice->GetSelection();
	std::shared_ptr<ControllerBase> device;
	if (selection >= 1 && selection <= (int)page_data.m_choice_entries.size())
	{
		const auto& e = page_data.m_choice_entries[selection - 1];
		device = e.instance;
		if (!device)
		{
			try
			{
				device = ControllerFactory::CreateController(e.api, e.uuid, e.name);
			}
			catch (const std::exception& ex)
			{
				set_status(wxString::FromUTF8(fmt::format("Can't use {}: {}", e.name, ex.what())), true);
				update_state();
				return;
			}
		}
	}
	select_device(device);
}

void InputSettings2::on_device_other(wxCommandEvent& event)
{
	auto& page_data = get_current_page_data();
	if (!page_data.m_controller)
		return;
	m_detector->cancel();

	std::vector<ControllerPtr> current;
	if (const auto device = page_data.m_controller->get_device())
		current.push_back(device);
	InputAPIAddWindow wnd(this, wxGetMousePosition() + wxSize(5, 5), current);
	if (wnd.ShowModal() != wxID_OK)
		return;
	wxASSERT(wnd.is_valid());
	select_device(wnd.get_controller());
}

void InputSettings2::select_device(std::shared_ptr<ControllerBase> device)
{
	auto& page_data = get_current_page_data();
	const auto& controller = page_data.m_controller;
	if (!controller)
		return;
	const auto previous = controller->get_device();
	if ((!previous && !device) || (previous && device && *previous == *device))
	{
		update_state();
		return;
	}

	// Dolphin keeps the bindings when the device changes (`X` now means the new device's X); the
	// device's defaults are offered (loaded right away when there are no bindings)
	const bool had_bindings = !controller->get_mapping_ids().empty();
	controller->set_device(device);
	if (!device)
		set_status(_("No device: only bindings that name a device explicitly work."));
	else if (!had_bindings)
	{
		const bool loaded = controller->load_default_bindings();
		set_status(loaded ? wxString::Format(_("%s selected, with its default bindings."), wxString::FromUTF8(device->display_name()))
			: wxString::Format(_("%s selected (no default bindings for this controller type)."), wxString::FromUTF8(device->display_name())));
	}
	else
	{
		const bool other_kind = !previous || previous->api() != device->api();
		const int answer = wxMessageBox(wxString::Format(_("Load the default bindings of %s?\n\nYes: replace this controller's bindings with the device's defaults.\nNo: keep the bindings; they now refer to %s (inputs it doesn't have show with ?)."),
			wxString::FromUTF8(device->display_name()), wxString::FromUTF8(device->display_name())),
			_("Device changed"), wxYES_NO | (other_kind ? wxYES_DEFAULT : wxNO_DEFAULT) | wxICON_QUESTION, this);
		if (answer == wxYES)
		{
			controller->load_default_bindings();
			set_status(wxString::Format(_("%s selected, with its default bindings."), wxString::FromUTF8(device->display_name())));
		}
		else
			set_status(wxString::Format(_("%s selected; the bindings were kept."), wxString::FromUTF8(device->display_name())));
	}
	update_state();
}

void InputSettings2::on_clear_bindings(wxCommandEvent& event)
{
	auto& page_data = get_current_page_data();
	if (!page_data.m_controller)
		return;
	if (wxMessageBox(_("Clear all bindings of this controller? The device stays selected."), _("Clear all bindings"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES)
		return;
	page_data.m_controller->clear_mappings();
	update_state();
}

void InputSettings2::on_load_preset(wxCommandEvent& event)
{
	auto& page_data = get_current_page_data();
	if (!page_data.m_controller)
		return;
	m_detector->cancel();
	const auto presets = InputPresets::ForType(page_data.m_controller->type());
	if (presets.empty())
	{
		set_status(_("No presets for this controller type."), true);
		return;
	}
	wxMenu menu;
	for (size_t i = 0; i < presets.size(); ++i)
	{
		auto* item = menu.Append(1 + (int)i, wxString::FromUTF8(presets[i].name));
		item->SetHelp(wxString::FromUTF8(presets[i].description));
	}
	const int choice = page_data.m_preset->GetPopupMenuSelectionFromUser(menu, 0, page_data.m_preset->GetSize().y);
	if (choice < 1 || choice > (int)presets.size())
		return;
	const auto& preset = presets[choice - 1];
	if ((page_data.m_controller->get_device() || !page_data.m_controller->get_mapping_ids().empty()) &&
		wxMessageBox(wxString::Format(_("Load the preset \"%s\"?\n\n%s\n\nIt replaces this controller's device and bindings."), wxString::FromUTF8(preset.name), wxString::FromUTF8(preset.description)),
			_("Load preset"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES)
		return;
	const auto result = InputPresets::Apply(*page_data.m_controller, preset.id);
	set_status(wxString::FromUTF8(result.message), !result.ok);
	update_state();
}

void InputSettings2::update_settings_mouse()
{
	// only while this window is the active one (not while a menu or another app has the mouse)
	UpdateSettingsMouse(IsActive());
}

void InputSettings2::device_defaults()
{
	auto& page_data = get_current_page_data();
	const auto device = page_data.m_controller ? page_data.m_controller->get_device() : nullptr;
	if (!device)
		return;
	m_detector->cancel();
	const bool loaded = page_data.m_controller->load_default_bindings();
	set_status(loaded ? wxString::Format(_("%s: default bindings loaded."), wxString::FromUTF8(device->display_name()))
		: wxString::Format(_("%s has no default bindings for this controller type: the bindings were cleared."), wxString::FromUTF8(device->display_name())));
	update_state();
}

void InputSettings2::device_settings(const std::shared_ptr<ControllerBase>& controller)
{
	switch (controller->api())
	{
	case InputAPI::DirectInput:
	case InputAPI::XInput:
	case InputAPI::GameCube:
	case InputAPI::WGIGamepad:
	case InputAPI::WGIRawController:
	case InputAPI::SDLController:
	case InputAPI::DSUClient:
	case InputAPI::RemoteGamePad:
	{
		DefaultControllerSettings wnd(this, wxGetMousePosition() + wxSize(5, 5), controller);
		wnd.ShowModal();
		break;
	}
	case InputAPI::Keyboard:
		break;
#ifdef SUPPORTS_WIIMOTE
	case InputAPI::Wiimote:
	{
		const auto wiimote = std::dynamic_pointer_cast<NativeWiimoteController>(controller);
		wxASSERT(wiimote);
		WiimoteControllerSettings wnd(this, wxGetMousePosition() + wxSize(5, 5), wiimote);
		wnd.ShowModal();
		break;
	}
#endif
	default:
		break;
	}
}

void InputSettings2::on_profile_dropdown(wxCommandEvent& event)
{
	auto* profile_names = dynamic_cast<wxComboBox*>(event.GetEventObject());
	wxASSERT(profile_names);
	wxWindowUpdateLocker lock(profile_names);
	
	const auto selected_value = profile_names->GetStringSelection();
	profile_names->Clear();

	for(const auto& profile : InputManager::get_profiles())
	{
		profile_names->Append(wxString::FromUTF8(profile));
	}

	profile_names->SetStringSelection(selected_value);
}

void InputSettings2::on_profile_text_changed(wxCommandEvent& event)
{
	auto* profile_names = dynamic_cast<wxComboBox*>(event.GetEventObject());
	wxASSERT(profile_names);

	auto& page_data = get_current_page_data();

	// load_bttn, save_bttn, delete_bttn, profile_status
	const auto text = event.GetString();
	const bool valid_name = InputManager::is_valid_profilename(text.utf8_string());
	const bool name_exists = profile_names->FindString(text) != wxNOT_FOUND;

	page_data.m_profile_load->Enable(name_exists);
	page_data.m_profile_save->Enable(valid_name);
	page_data.m_profile_delete->Enable(name_exists);
	page_data.m_profile_status->Hide();
}

void InputSettings2::on_profile_save(wxCommandEvent& event)
{
	auto& page_data = get_current_page_data();

	auto* profile_names = page_data.m_profiles;
	auto* text = page_data.m_profile_status;

	const auto selection = profile_names->GetValue().utf8_string();
	text->Show();
	if (selection.empty() || !InputManager::is_valid_profilename(selection))
	{
		text->SetLabelText(_("invalid profile name"));
		text->SetForegroundColour(wxTheColourDatabase->Find("ERROR"));
		text->Refresh();
		return;
	}

	if(InputManager::instance().save(m_notebook->GetSelection(), selection))
	{
		text->SetLabelText(_("profile saved"));
		text->SetForegroundColour(wxTheColourDatabase->Find("SUCCESS"));
	}
	else
	{
		text->SetLabelText(_("couldn't save profile"));
		text->SetForegroundColour(wxTheColourDatabase->Find("ERROR"));
	}

	text->Refresh();
}

void InputSettings2::on_profile_delete(wxCommandEvent& event)
{
	auto& page_data = get_current_page_data();

	auto* profile_names = page_data.m_profiles;
	auto* text = page_data.m_profile_status;

	const auto selection = profile_names->GetStringSelection().utf8_string();

	text->Show();
	if (selection.empty() || !InputManager::is_valid_profilename(selection))
	{
		text->SetLabelText(_("invalid profile name"));
		text->SetForegroundColour(wxTheColourDatabase->Find("ERROR"));
		text->Refresh();
		return;
	}
	try
	{
		const fs::path old_path = ActiveSettings::GetConfigPath("controllerProfiles/{}.txt", selection);
		fs::remove(old_path);

		const fs::path path = ActiveSettings::GetConfigPath("controllerProfiles/{}.xml", selection);
		fs::remove(path);

		profile_names->ChangeValue(kDefaultProfileName);
		text->SetLabelText(_("profile deleted"));
		text->SetForegroundColour(wxTheColourDatabase->Find("SUCCESS"));

		page_data.m_profile_load->Disable();
		page_data.m_profile_save->Disable();
		page_data.m_profile_delete->Disable();
	}
	catch (const std::exception&)
	{
		text->SetLabelText(_("can't delete profile"));
		text->SetForegroundColour(wxTheColourDatabase->Find("ERROR"));
	}
	text->Refresh();
}


void InputSettings2::on_profile_load(wxCommandEvent& event)
{
	auto& page_data = get_current_page_data();

	auto* profile_names = page_data.m_profiles;
	auto* text = page_data.m_profile_status;

	const auto selection = profile_names->GetValue().utf8_string();
	text->Show();
	if (selection.empty() || !InputManager::is_valid_profilename(selection))
	{
		text->SetLabelText(_("invalid profile name"));
		text->SetForegroundColour(wxTheColourDatabase->Find("ERROR"));
		text->Refresh();
		return;
	}

	const auto page_index = m_notebook->GetSelection();
	if (InputManager::instance().load(page_index, selection))
	{
		text->SetLabelText(_("profile loaded"));
		text->SetForegroundColour(wxTheColourDatabase->Find("SUCCESS"));
	}
	else
	{
		text->SetLabelText(_("couldn't load profile"));
		text->SetForegroundColour(wxTheColourDatabase->Find("ERROR"));
	}

	text->Refresh();

	// update controller info
	page_data.m_controller = InputManager::instance().get_controller(page_index);
	update_state();
}

void InputSettings2::on_emulated_controller_dropdown(wxCommandEvent& event)
{
	auto* emulated_controllers = dynamic_cast<wxComboBox*>(event.GetEventObject());
	wxASSERT(emulated_controllers);

	wxWindowUpdateLocker lock(emulated_controllers);

	bool is_gamepad_selected = false;
	bool is_wpad_selected = false;
	const auto selected = emulated_controllers->GetSelection();
	const auto selected_value = emulated_controllers->GetStringSelection();
	if(selected != wxNOT_FOUND)
	{
		is_gamepad_selected = selected_value == wxString::FromUTF8(EmulatedController::type_to_string(EmulatedController::Type::VPAD));
		is_wpad_selected = !is_gamepad_selected && selected != 0;
	}

	const auto [vpad_count, wpad_count] = InputManager::instance().get_controller_count();

	emulated_controllers->Clear();
	emulated_controllers->AppendString(_("Disabled"));

	if (vpad_count < InputManager::kMaxVPADControllers || is_gamepad_selected)
		emulated_controllers->Append(wxString::FromUTF8(EmulatedController::type_to_string(EmulatedController::Type::VPAD)));

	if (wpad_count < InputManager::kMaxWPADControllers || is_wpad_selected)
	{
		emulated_controllers->AppendString(wxString::FromUTF8(EmulatedController::type_to_string(EmulatedController::Type::Pro)));
		emulated_controllers->AppendString(wxString::FromUTF8(EmulatedController::type_to_string(EmulatedController::Type::Classic)));
		emulated_controllers->AppendString(wxString::FromUTF8(EmulatedController::type_to_string(EmulatedController::Type::Wiimote)));

		// only one Balance Board (it always uses WPAD channel 3)
		const auto board_name = wxString::FromUTF8(EmulatedController::type_to_string(EmulatedController::Type::BalanceBoard));
		bool other_board = false;
		for (size_t i = 0; i < InputManager::kMaxController; ++i)
		{
			if (i == (size_t)m_notebook->GetSelection())
				continue;
			if (const auto c = InputManager::instance().get_controller(i); c && c->type() == EmulatedController::Type::BalanceBoard)
				other_board = true;
		}
		if (!other_board || selected_value == board_name)
			emulated_controllers->AppendString(board_name);
	}

	emulated_controllers->SetStringSelection(selected_value);
}
