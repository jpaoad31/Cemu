#include "input/emulated/SingleDeviceMigration.h"

#include "input/emulated/InputExpression.h"
#include "input/api/Mouse/MouseInputs.h"

#include <algorithm>

namespace SingleDeviceMigration
{
	namespace
	{
		// the profile's devices; input names aren't known here (they stay as written, `#id` stays
		// `#id` and becomes the name when the profile is loaded against the real device)
		class ProfileDevices : public InputExpr::DeviceSet
		{
		public:
			ProfileDevices(const std::vector<ProfileDevice>& devices, size_t default_device) : m_devices(devices), m_default(default_device) {}
			size_t default_device() const override { return m_default; }
			size_t device_count() const override { return m_devices.size(); }
			std::string device_api(size_t dev) const override { return m_devices[dev].api; }
			std::string device_uuid(size_t dev) const override { return m_devices[dev].uuid; }
			std::string device_display_name(size_t dev) const override { return m_devices[dev].display_name; }
			std::string input_name(size_t, InputExpr::InputId) const override { return {}; }
			std::optional<InputExpr::InputId> find_input(size_t, std::string_view) const override { return std::nullopt; }

		private:
			const std::vector<ProfileDevice>& m_devices;
			size_t m_default;
		};

		constexpr size_t kKeyboardGroup = (size_t)-2;

		// devices that are one now: the keyboard and the mouse
		size_t group_of(const std::vector<ProfileDevice>& devices, size_t dev)
		{
			if (dev >= devices.size())
				return InputExpr::kNoDevice;
			return IsKeyboardOrMouse(devices[dev].api) ? kKeyboardGroup : dev;
		}

		std::string group_name(const std::vector<ProfileDevice>& devices, size_t group)
		{
			if (group == kKeyboardGroup)
				return std::string(kKeyboardMouseName);
			return devices[group].display_name.empty() ? devices[group].api + "/" + devices[group].uuid : devices[group].display_name;
		}

		int api_rank(std::string_view api)
		{
			if (api == "RemoteGamePad")
				return 2;
			if (api == "Wiimote")
				return 1;
			return 0;
		}

		// the old Mouse device's input -> the Keyboard/Mouse device's name
		void rename_mouse_input(InputExpr::Leaf& leaf)
		{
			std::optional<uint64_t> id;
			if (leaf.input_text.size() > 1 && leaf.input_text[0] == '#' && leaf.input && MouseInputs::IsValid(*leaf.input))
				id = *leaf.input;
			else
				id = MouseInputs::FromName(leaf.input_text);
			if (!id)
				return;
			leaf.input_text = MouseInputs::CombinedName(*id);
			leaf.input.reset();
		}

		std::map<size_t, size_t> inputs_per_device(const std::vector<ProfileDevice>& devices, const std::map<uint64_t, std::string>& bindings)
		{
			const ProfileDevices set(devices, InputExpr::kNoDevice);
			std::map<size_t, size_t> count;
			for (const auto& [id, text] : bindings)
			{
				for (const auto& leaf : InputExpr::Expression::parse(text, set).leaves())
				{
					if (leaf.device != InputExpr::kNoDevice)
						++count[leaf.device];
				}
			}
			return count;
		}
	}

	bool IsKeyboardOrMouse(std::string_view api)
	{
		return api == "Keyboard" || api == "Mouse";
	}

	size_t Result::dropped_inputs() const
	{
		size_t n = 0;
		for (const auto& d : dropped)
			n += d.inputs;
		return n;
	}

	std::optional<size_t> ChooseDevice(const std::vector<ProfileDevice>& devices, const std::map<uint64_t, std::string>& bindings)
	{
		if (devices.empty())
			return std::nullopt;
		const auto count = inputs_per_device(devices, bindings);
		std::optional<size_t> best;
		for (size_t i = 0; i < devices.size(); ++i)
		{
			if (IsKeyboardOrMouse(devices[i].api))
				continue;
			if (!best)
			{
				best = i;
				continue;
			}
			const auto key = [&](size_t d) {
				const auto it = count.find(d);
				return std::make_pair(it == count.end() ? (size_t)0 : it->second, api_rank(devices[d].api));
			};
			if (key(i) > key(*best))
				best = i;
		}
		if (best)
			return best;
		// the keyboard (or the mouse when it was alone)
		for (size_t i = 0; i < devices.size(); ++i)
		{
			if (devices[i].api == "Keyboard")
				return i;
		}
		return 0;
	}

	Result Migrate(const std::vector<ProfileDevice>& devices, const std::map<uint64_t, std::string>& bindings)
	{
		Result result;
		const auto kept = ChooseDevice(devices, bindings);
		if (!kept)
		{
			result.bindings = bindings;
			return result;
		}
		const size_t kept_group = group_of(devices, *kept);
		if (kept_group == kKeyboardGroup)
			result.device = ProfileDevice{"Keyboard", "keyboard", std::string(kKeyboardMouseName)};
		else
			result.device = devices[*kept];

		const ProfileDevices parse_set(devices, InputExpr::kNoDevice);
		const ProfileDevices print_set(devices, *kept);
		std::map<std::string, size_t> dropped; // per device name
		for (size_t g = 0; g < devices.size(); ++g)
		{
			const size_t group = group_of(devices, g);
			if (group != kept_group)
				dropped.emplace(group_name(devices, group), 0);
		}

		for (const auto& [id, text] : bindings)
		{
			auto expr = InputExpr::Expression::parse(text, parse_set);
			if (!expr.ok())
			{
				result.bindings[id] = text; // kept as text, inactive (logged when loaded)
				continue;
			}
			if (expr.is_list())
			{
				expr = InputExpr::Expression::remove_leaves(expr, [&](const InputExpr::Leaf& leaf) {
					const size_t group = group_of(devices, leaf.device);
					if (group == kept_group)
						return false;
					++dropped[group == InputExpr::kNoDevice ? leaf.device_text : group_name(devices, group)];
					return true;
				}, parse_set);
			}
			else
			{
				const auto leaves = expr.leaves();
				if (std::any_of(leaves.cbegin(), leaves.cend(), [&](const InputExpr::Leaf& l) { return group_of(devices, l.device) != kept_group; }))
					++result.advanced_kept;
			}
			if (expr.empty())
				continue;

			expr = InputExpr::Expression::map_leaves(expr, [&](InputExpr::Leaf& leaf) {
				const bool mouse = leaf.device < devices.size() && devices[leaf.device].api == "Mouse";
				if (mouse)
					rename_mouse_input(leaf);
				if (group_of(devices, leaf.device) == kept_group)
				{
					leaf.qualified = false;
					leaf.device_text.clear();
				}
				else if (mouse)
				{
					// the mouse of a controller whose device is another one: explicit Keyboard/Mouse
					leaf.device = InputExpr::kNoDevice;
					leaf.device_text = "Keyboard/keyboard";
				}
			}, print_set);
			result.bindings[id] = expr.text();
		}

		for (const auto& [name, inputs] : dropped)
			result.dropped.push_back({name, inputs});
		return result;
	}

	std::string Note(const Result& result)
	{
		std::string names;
		for (const auto& d : result.dropped)
		{
			if (d.inputs == 0)
				continue;
			if (!names.empty())
				names += ", ";
			names += d.device + " (" + std::to_string(d.inputs) + ")";
		}
		if (names.empty())
			return {};
		return "Bindings from " + names + " were removed: one device per controller.";
	}
}
