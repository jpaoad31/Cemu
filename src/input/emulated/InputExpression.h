#pragma once

// Input bindings as expressions (fork addition, see INPUT_REWORK.md).
//
// Every emulated control (a GamePad button, a stick direction, the Balance Board's Power button...)
// maps to an expression over the inputs of the devices attached to the emulated controller:
//
//   `Keyboard/keyboard:X` | `RemoteGamePad/26770:Button 1` | `SDLController/0_0300...:a`
//
// The design follows Dolphin's ControlReference expressions (`|`, `&`, `!`, `+`, functions, inputs
// written as `Device:Input`), but this is our own code and grammar (Cemu is MPL-2.0, Dolphin GPL).
//
// Inputs are written in backticks as `API/uuid:Input` (or `API:Input` for the first device of that
// API). Input is the device's input name (Controller::get_button_name) or #<id>.
//
// Values: inputs give 0..1 (buttons 0/1, stick directions and triggers their deflection).
// Operators, lowest precedence first:
//   a | b    either: value of the stronger (larger magnitude), active if either is
//   a ^ b    exactly one of them is active
//   a & b    both: the weaker value, active if both are
//   a < b, a > b    1 or 0
//   a + b, a - b
//   a * b, a / b
//   !a (1 - a, active when a isn't), -a
// Functions: min(a,b) max(a,b) clamp(x,lo,hi) abs(x) deadzone(x,dz) toggle(x) hold(x,seconds)
//   pulse(x,seconds) timer(seconds)
// A button is "down" when its expression is active: an input is active when the device reports it
// pressed (stick directions past the device's dead zone), a computed value when it is above 0.5.
//
// No Cemu dependencies (standard headers only), so the parser can be tested on its own
// (InputExpressionTest.cpp).

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace InputExpr
{
	using InputId = uint64_t;
	constexpr size_t kNoDevice = (size_t)-1;

	// the devices an expression can refer to: the emulated controller's device list
	class DeviceSet
	{
	public:
		virtual ~DeviceSet() = default;
		virtual size_t device_count() const = 0;
		virtual std::string device_api(size_t dev) const = 0;  // "Keyboard", "SDLController", ...
		virtual std::string device_uuid(size_t dev) const = 0; // "keyboard", "0_03000000..."
		virtual std::string device_display_name(size_t dev) const = 0;
		virtual std::string input_name(size_t dev, InputId id) const = 0;
		virtual std::optional<InputId> find_input(size_t dev, std::string_view name) const = 0;
	};

	// live input values
	class InputSource
	{
	public:
		virtual ~InputSource() = default;
		virtual float value(size_t dev, InputId id) const = 0; // 0..1
		virtual bool active(size_t dev, InputId id) const = 0;
	};

	struct Leaf
	{
		std::string device_text; // as written, e.g. "Keyboard/keyboard"
		std::string input_text;  // as written, e.g. "X" or "#12"
		size_t device = kNoDevice;
		std::optional<InputId> input;

		bool resolved() const { return device != kNoDevice && input.has_value(); }
	};

	struct Node;

	class Expression
	{
	public:
		Expression() = default;

		// parses text against the device list. On a syntax error the expression keeps the text (so
		// nothing is lost when saving) but evaluates to 0; error() says why. Inputs whose device isn't
		// in the list, or whose name the device doesn't know, are kept unresolved (see warnings()).
		static Expression parse(std::string_view text, const DeviceSet& devices);
		// a single input
		static Expression from_input(size_t device, InputId id, const DeviceSet& devices);

		bool empty() const { return m_text.empty(); }
		bool ok() const { return m_error.empty(); }
		const std::string& error() const { return m_error; }
		// unresolved inputs ("device not attached: ...", "unknown input ...")
		std::vector<std::string> warnings() const;

		float value(const InputSource& source) const;
		bool active(const InputSource& source) const;

		// canonical text (for saving), from the parsed tree; the original text if it didn't parse
		const std::string& text() const { return m_text; }
		// short text for the UI: input names, with the device name when with_device is true
		std::string display(const DeviceSet& devices, bool with_device) const;

		// a plain list of inputs OR'ed together (or empty): what the simple UI edits
		bool is_list() const;
		std::vector<Leaf> leaves() const;
		bool references_device(size_t dev) const;

		// list editing; for an expression that isn't a list, append wraps it: (expr) | input
		static Expression append(const Expression& expr, size_t device, InputId id, const DeviceSet& devices);
		// removes the inputs of a device from a list (an expression that isn't a list is kept as is)
		static Expression remove_device(const Expression& expr, size_t device, const DeviceSet& devices);

	private:
		std::shared_ptr<Node> m_root;
		std::string m_text;
		std::string m_error;
	};

	// helpers shared with the profile code and the tests
	std::string quote_input(std::string_view device_text, std::string_view input_text);
	std::string device_key(const DeviceSet& devices, size_t dev); // "API/uuid"
	std::string input_key(const DeviceSet& devices, size_t dev, InputId id); // name if it maps back, else #id
}
