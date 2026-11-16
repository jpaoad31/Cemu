#pragma once

// "Detect" capture for the input settings (fork addition, see INPUT_REWORK.md), after Dolphin's
// detect-with-timeout: after a click on a binding field, the first input that changes from its
// state at the click becomes the binding (or is added to it). 3 seconds, Escape cancels. It listens
// to the emulated controller's selected device only, as Dolphin does; with "All devices" also to
// the devices bindings name, Keyboard/Mouse and every connected SDL gamepad, and an input of
// another device than the selected one is written with its device (explicit).
//
// Why not wx key events in a focused text field (the old way): on macOS a text field doesn't
// always hold the focus, keys pressed with Command never send a key-up (a "stuck" key blocked the
// old "wait until idle" logic for good), and only the device picked in the "Controller" box was
// watched. Here keys come from the dialog's char hook (raw key codes, before any control sees
// them) and from Cemu's own keyboard state, and new presses are compared with a snapshot taken at
// the click, so keys held or stuck at that moment don't matter.

#include "input/emulated/EmulatedController.h"
#include "input/api/Controller.h"

#include <wx/event.h>
#include <chrono>
#include <functional>

class MappingControl;

class InputDetector
{
public:
	static constexpr std::chrono::milliseconds kTimeout{3000};

	struct Result
	{
		bool detected = false;
		bool other_device = false; // the input came from another device than the selected one
		std::string message;       // for a status line ("X", "A [Remote GamePad] (explicit)", "No input detected")
	};
	using Callback = std::function<void(const Result&)>;

	// the dialog that owns the detector (one input settings window at a time)
	static InputDetector* Get() { return s_instance; }
	InputDetector();
	~InputDetector();

	// expression_only: report the input through on_input instead of changing the binding
	// all_devices: Dolphin's "All devices" detection (else the selected device only)
	void start(const EmulatedControllerPtr& controller, uint64 mapping, bool append, MappingControl* control, Callback callback, bool all_devices = false);
	void start_for_input(const EmulatedControllerPtr& controller, std::function<void(const ControllerPtr&, uint64)> on_input, Callback callback, bool all_devices = true);

	// the devices Detect listens to: the selected device; with all_devices also the extra devices,
	// then the others (deduplicated, the selected device first)
	static std::vector<ControllerPtr> candidate_devices(const ControllerPtr& selected, const std::vector<ControllerPtr>& extra, const std::vector<ControllerPtr>& others, bool all_devices);
	void cancel(const std::string& message = {});
	bool is_active() const { return m_active; }
	// the settings window's "All devices" option, for the binding fields' Detect
	void set_all_devices(bool all) { m_all_devices = all; }
	bool all_devices() const { return m_all_devices; }
	MappingControl* control() const { return m_control; }

	// called by the dialog's timer
	void poll();
	// the dialog's wxEVT_CHAR_HOOK: true if the key was used (don't pass it on)
	bool on_char_hook(wxKeyEvent& event);
	// the control is being destroyed
	void forget_control(MappingControl* control);

	// refreshes a device's state for the settings window (not the Remote GamePad while a game
	// runs: the game's reads own its quick-tap latches)
	static bool update_device(ControllerBase& controller);

private:
	struct Candidate
	{
		ControllerPtr controller;
		std::vector<uint32> baseline; // pressed at the start (or still held since)
	};

	void finish(const ControllerPtr& controller, uint64 input);

	static InputDetector* s_instance;

	bool m_active = false;
	bool m_all_devices = false;
	EmulatedControllerPtr m_controller;
	uint64 m_mapping = 0;
	bool m_append = false;
	MappingControl* m_control = nullptr;
	Callback m_callback;
	std::function<void(const ControllerPtr&, uint64)> m_on_input;
	std::chrono::steady_clock::time_point m_start;
	std::vector<Candidate> m_candidates;
	std::optional<uint32> m_key; // raw key code from the char hook
};
