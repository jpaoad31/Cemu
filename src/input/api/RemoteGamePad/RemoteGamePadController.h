#pragma once

#include "input/api/Controller.h"
#include "input/api/RemoteGamePad/RemoteGamePadControllerProvider.h"

// One phone per UDP port. Raw button N is bit N of the protocol's button field (Wii U GamePad
// buttons, already mapped by position on the phone), so VPAD gets a default mapping 1:1.
class RemoteGamePadController : public Controller<RemoteGamePadControllerProvider>
{
public:
	RemoteGamePadController();
	RemoteGamePadController(const RemoteGamePadProviderSettings& settings);

	std::string_view api_name() const override
	{
		static_assert(to_string(InputAPI::RemoteGamePad) == "RemoteGamePad");
		return to_string(InputAPI::RemoteGamePad);
	}
	InputAPI::Type api() const override { return InputAPI::RemoteGamePad; }

	void save(pugi::xml_node& node) override;
	void load(const pugi::xml_node& node) override;

	bool connect() override;
	bool is_connected() override;

	bool has_motion() override { return true; }
	MotionSample get_motion_sample() override;

	bool has_position() override;
	glm::vec2 get_position() override;
	glm::vec2 get_prev_position() override;
	PositionVisibility GetPositionVisibility() override;

	std::string get_button_name(uint64 button) const override;

	std::string get_status_text() const { return m_provider->get_status_text(); }

	void on_emulated_read_done() override;

protected:
	ControllerState raw_state() override;

private:
	// the state the last update_state() (i.e. the current game read) used, latches included
	RemoteGamePadControllerProvider::State read_state() const;
	mutable std::mutex m_read_mutex;
	RemoteGamePadControllerProvider::State m_read_state{};
	bool m_have_read_state = false;
	uint32 m_reported_latch = 0;     // latched buttons reported since the last read-done
	bool m_reported_touch = false;
	uint32 m_reported_touch_seq = 0;

	glm::vec2 m_prev_position{};
	glm::vec2 m_position{};
};
