#pragma once

#include "input/emulated/WPADController.h"
#include "input/emulated/MouseBindings.h"

class WiimoteController : public WPADController
{
	using base_type = WPADController;
public:
	enum ButtonId
	{
		kButtonId_None,

		kButtonId_A,
		kButtonId_B,
		kButtonId_1,
		kButtonId_2,

		kButtonId_Nunchuck_Z,
		kButtonId_Nunchuck_C,

		kButtonId_Plus,
		kButtonId_Minus,


		kButtonId_Up,
		kButtonId_Down,
		kButtonId_Left,
		kButtonId_Right,


		kButtonId_Nunchuck_Up,
		kButtonId_Nunchuck_Down,
		kButtonId_Nunchuck_Left,
		kButtonId_Nunchuck_Right,

		kButtonId_Home,

		kButtonId_Max,

		// fork: the pointer (IR) group, like Dolphin's Point (MouseBindings.h); outside the button loop
		kButtonId_Pointer_Up = MouseBindings::kPointerUp,
		kButtonId_Pointer_Down = MouseBindings::kPointerDown,
		kButtonId_Pointer_Left = MouseBindings::kPointerLeft,
		kButtonId_Pointer_Right = MouseBindings::kPointerRight,
		kButtonId_Pointer_Hide = MouseBindings::kPointerHide,
		kButtonId_Pointer_Recenter = MouseBindings::kPointerRecenter,
	};

	WiimoteController(size_t player_index);

	Type type() const override { return Type::Wiimote; }
	WPADDeviceType get_device_type() const override { return m_device_type; }
	void set_device_type(WPADDeviceType device_type);

	bool is_mpls_attached() override;

	uint32 get_emulated_button_flag(uint32 id) const override;
	size_t get_highest_mapping_id() const override { return kButtonId_Max; }
	bool is_axis_mapping(uint64 mapping) const override
	{
		return (mapping >= kButtonId_Nunchuck_Up && mapping <= kButtonId_Nunchuck_Right)
			|| (mapping >= kButtonId_Pointer_Up && mapping <= kButtonId_Pointer_Right);
	}

	// Pointer settings (Dolphin's Point group): where the binding's -1..1 range lands on the screen
	struct PointerSettings
	{
		float center_x = 0, center_y = 0; // offset, in screen halves (-1..1)
		float width = 1, height = 1;      // 1 = the binding's full range covers the screen
		bool relative = false;            // the binding moves the pointer (speed) instead of placing it
		bool auto_hide = false;           // hide it after 2 s without movement
	};
	PointerSettings get_pointer_settings() const;
	void set_pointer_settings(const PointerSettings& settings);

	// the pointer from the bindings (0..1, top left origin); also steps the relative mode
	struct PointerState
	{
		bool bound = false;
		bool visible = false;
		glm::vec2 position{};
	};
	PointerState get_pointer();
	PointerSample get_kpad_pointer() override;

	std::vector<std::pair<uint64, uint64>> get_default_mapping(const std::shared_ptr<ControllerBase>& controller) const override;

	glm::vec2 get_axis() const override;
	glm::vec2 get_rotation() const override;
	glm::vec2 get_trigger() const override;

	void load(const pugi::xml_node& node) override;
	void save(pugi::xml_node& node) override;
	
	static uint32 s_get_emulated_button_flag(uint32 id);

	static std::string_view get_button_name(ButtonId id);

	bool is_start_down() const override { return is_mapping_down(kButtonId_Plus); }
	bool is_left_down() const override { return is_mapping_down(kButtonId_Left); }
	bool is_right_down() const override { return is_mapping_down(kButtonId_Right); }
	bool is_up_down() const override { return is_mapping_down(kButtonId_Up); }
	bool is_down_down() const override { return is_mapping_down(kButtonId_Down); }
	bool is_a_down() const override { return is_mapping_down(kButtonId_A); }
	bool is_b_down() const override { return is_mapping_down(kButtonId_B); }
	bool is_home_down() const override { return is_mapping_down(kButtonId_Home); }

private:
	WPADDeviceType m_device_type = kWAPDevCore;

	mutable std::mutex m_pointer_mutex;
	PointerSettings m_pointer_settings;
	glm::vec2 m_pointer_relative{};  // relative mode: the pointer, -1..1
	glm::vec2 m_pointer_last{};      // last position (auto hide, previous for KPAD)
	glm::vec2 m_pointer_prev{};
	std::chrono::steady_clock::time_point m_pointer_step{}, m_pointer_moved{};
};
