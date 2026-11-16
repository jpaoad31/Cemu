#pragma once

#include "input/emulated/EmulatedController.h"
#include "input/emulated/MouseBindings.h"
#include "Cafe/OS/libs/vpad/vpad.h"


class VPADController : public EmulatedController
{
public:
	enum ButtonId
	{
		kButtonId_None,

		kButtonId_A,
		kButtonId_B,
		kButtonId_X,
		kButtonId_Y,

		kButtonId_L,
		kButtonId_R,
		kButtonId_ZL,
		kButtonId_ZR,

		kButtonId_Plus,
		kButtonId_Minus,

		kButtonId_Up,
		kButtonId_Down,
		kButtonId_Left,
		kButtonId_Right,

		kButtonId_StickL,
		kButtonId_StickR,

		kButtonId_StickL_Up,
		kButtonId_StickL_Down,
		kButtonId_StickL_Left,
		kButtonId_StickL_Right,

		kButtonId_StickR_Up,
		kButtonId_StickR_Down,
		kButtonId_StickR_Left,
		kButtonId_StickR_Right,

		kButtonId_Mic,
		kButtonId_Screen,

		kButtonId_Home,

		kButtonId_Max,

		// fork: explicit touch and tilt groups (MouseBindings.h); not GamePad buttons, outside the
		// 1..kButtonId_Max button loop
		kButtonId_Touch_Up = MouseBindings::kTouchUp,
		kButtonId_Touch_Down = MouseBindings::kTouchDown,
		kButtonId_Touch_Left = MouseBindings::kTouchLeft,
		kButtonId_Touch_Right = MouseBindings::kTouchRight,
		kButtonId_Touch_Press = MouseBindings::kTouchPress,

		kButtonId_Tilt_Up = MouseBindings::kTiltUp,
		kButtonId_Tilt_Down = MouseBindings::kTiltDown,
		kButtonId_Tilt_Left = MouseBindings::kTiltLeft,
		kButtonId_Tilt_Right = MouseBindings::kTiltRight,
		kButtonId_Turn_Left = MouseBindings::kTurnLeft,
		kButtonId_Turn_Right = MouseBindings::kTurnRight,
		kButtonId_Tilt_Hold = MouseBindings::kTiltHold,
	};

	using EmulatedController::EmulatedController;

	Type type() const override { return VPAD; }

	void VPADRead(VPADStatus_t& status, const BtnRepeat& repeat);

	void update() override;

	uint32 get_emulated_button_flag(uint32 id) const override;

	glm::vec2 get_axis() const override;
	glm::vec2 get_rotation() const override;
	glm::vec2 get_trigger() const override;

	bool is_mic_active() { return m_mic_active; }
	bool is_screen_active() { return m_screen_active; }
	bool is_screen_active_toggle() { return m_screen_active_toggle; }
	void set_screen_toggle(bool toggle) {m_screen_active_toggle = toggle;}

	static std::string_view get_button_name(ButtonId id);

	void clear_rumble();
	bool push_rumble(uint8* pattern, uint8 length);

	size_t get_highest_mapping_id() const override { return kButtonId_Max; }
	bool is_axis_mapping(uint64 mapping) const override
	{
		return (mapping >= kButtonId_StickL_Up && mapping <= kButtonId_StickR_Right)
			|| (mapping >= kButtonId_Touch_Up && mapping <= kButtonId_Touch_Right)
			|| (mapping >= kButtonId_Tilt_Up && mapping <= kButtonId_Turn_Right);
	}

	// the touch screen: from a device with touch (Remote GamePad: wins while the phone is touched),
	// else from the Touch bindings
	struct TouchState
	{
		bool bound = false;       // the Touch group has bindings
		bool device = false;      // a device (phone) is touching
		bool touching = false;
		glm::vec2 position{};     // 0..1, top left origin (also while not touching, for the UI)
	};
	TouchState get_touch() const;

	// the emulated tilt (no motion device): the Tilt group's bindings, -1..1 each
	struct TiltState
	{
		bool bound = false;  // the Tilt group has bindings
		bool active = false; // bound, and Hold (if bound) is active
		glm::vec2 tilt{};    // x: right, y: down (screen directions of the default mouse drag)
		float turn = 0;      // + left
	};
	TiltState get_tilt() const;

	bool is_start_down() const override { return is_mapping_down(kButtonId_Plus); }
	bool is_left_down() const override { return is_mapping_down(kButtonId_Left); }
	bool is_right_down() const override { return is_mapping_down(kButtonId_Right); }
	bool is_up_down() const override { return is_mapping_down(kButtonId_Up); }
	bool is_down_down() const override { return is_mapping_down(kButtonId_Down); }
	bool is_a_down() const override { return is_mapping_down(kButtonId_A); }
	bool is_b_down() const override { return is_mapping_down(kButtonId_B); }
	bool is_home_down() const override { return is_mapping_down(kButtonId_Home); }

	std::vector<std::pair<uint64, uint64>> get_default_mapping(const std::shared_ptr<ControllerBase>& controller) const override;

	void load(const pugi::xml_node& node) override;
	void save(pugi::xml_node& node) override;

private:
	bool m_mic_active = false;
	bool m_screen_active = false;
	bool m_screen_active_toggle = false;
	uint32be m_last_holdvalue = 0;

	std::chrono::high_resolution_clock::time_point m_last_hold_change{}, m_last_pulse{};

	std::mutex m_rumble_mutex;
	std::chrono::high_resolution_clock::time_point m_last_rumble_check{};
	std::queue<std::vector<bool>> m_rumble_queue;
	uint8 m_parser = 0;

	void update_touch(VPADStatus_t& status);
	void update_motion(VPADStatus_t& status);
	glm::ivec2 m_last_touch_position{};
	// emulated tilt (game thread)
	glm::vec3 m_last_gyro_rotation{};
	float m_turn = 0; // degrees
	std::chrono::steady_clock::time_point m_last_tilt{};
};
