// Standalone test of the binding expressions and the profile bindings (old -> new format).
// Not in CMake (Cemu has no test target). Build and run:
//
//   P=build/vcpkg_installed/arm64-osx
//   clang++ -std=c++20 -Wall -I src -I $P/include src/input/emulated/InputExpressionTest.cpp \
//     src/input/emulated/InputExpression.cpp src/input/emulated/InputProfileBindings.cpp \
//     src/input/emulated/MouseBindings.cpp src/input/emulated/SingleDeviceMigration.cpp \
//     $P/lib/libpugixml.a -o /tmp/exprtest && /tmp/exprtest

#include "input/emulated/InputExpression.h"
#include "input/emulated/InputProfileBindings.h"
#include "input/emulated/MouseBindings.h"
#include "input/emulated/SingleDeviceMigration.h"
#include "input/api/Mouse/MouseInputs.h"

#include <pugixml.hpp>

#include <cmath>
#include <cstdio>
#include <map>
#include <sstream>
#include <thread>

using namespace InputExpr;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)
#define CHECK_EQ(a, b) do { const auto _a = (a); const auto _b = (b); if (!(_a == _b)) { std::printf("FAIL %s:%d: %s == %s\n  got:      %s\n  expected: %s\n", __FILE__, __LINE__, #a, #b, to_s(_a).c_str(), to_s(_b).c_str()); g_failures++; } } while (0)

static std::string to_s(const std::string& s) { return s; }
[[maybe_unused]] static std::string to_s(const char* s) { return s; }
[[maybe_unused]] static std::string to_s(bool b) { return b ? "true" : "false"; }
static std::string to_s(size_t v) { return std::to_string(v); }
[[maybe_unused]] static std::string to_s(int v) { return std::to_string(v); }

// three devices like a real setup: the keyboard, an SDL pad, the Remote GamePad
struct FakeDevices : DeviceSet, InputSource
{
	struct Dev
	{
		std::string api, uuid, name;
		std::map<InputId, std::string> inputs;
	};
	std::vector<Dev> devs;
	std::map<std::pair<size_t, InputId>, float> values;
	size_t def = kNoDevice; // the selected device
	size_t default_device() const override { return def; }

	FakeDevices()
	{
		Dev kb{"Keyboard", "keyboard", "Keyboard", {}};
		for (int i = 0; i < 26; i++)
			kb.inputs[i] = std::string(1, (char)('A' + i));
		kb.inputs[49] = "Space";
		kb.inputs[50] = "`"; // the grave key: needs escaping
		Dev sdl{"SDLController", "0_030000005e040000", "Pro Controller", {{0, "a"}, {1, "b"}, {2, "x"}, {3, "y"}, {40, "X-Axis+"}}};
		Dev rgp{"RemoteGamePad", "26770", "Remote GamePad (UDP 26770)", {}};
		for (int i = 0; i < 32; i++)
			rgp.inputs[i] = "Button " + std::to_string(i);
		devs = {kb, sdl, rgp};
	}

	size_t device_count() const override { return devs.size(); }
	std::string device_api(size_t d) const override { return devs[d].api; }
	std::string device_uuid(size_t d) const override { return devs[d].uuid; }
	std::string device_display_name(size_t d) const override { return devs[d].name; }
	std::string input_name(size_t d, InputId id) const override
	{
		const auto it = devs[d].inputs.find(id);
		return it == devs[d].inputs.end() ? "Button " + std::to_string(id) : it->second;
	}
	std::optional<InputId> find_input(size_t d, std::string_view name) const override
	{
		for (const auto& [id, n] : devs[d].inputs)
		{
			if (n.size() == name.size() && std::equal(n.begin(), n.end(), name.begin(), [](char a, char b) { return std::tolower(a) == std::tolower(b); }))
				return id;
		}
		return std::nullopt;
	}
	float value(size_t d, InputId id) const override
	{
		const auto it = values.find({d, id});
		return it == values.end() ? 0.0f : it->second;
	}
	bool active(size_t d, InputId id) const override { return value(d, id) > 0.1f; }

	void set(size_t d, InputId id, float v) { values[{d, id}] = v; }
	void clear() { values.clear(); }
};

static void test_parse_and_print()
{
	FakeDevices dev;
	auto e = Expression::parse("`Keyboard/keyboard:X` | `RemoteGamePad/26770:Button 1`", dev);
	CHECK(e.ok());
	CHECK(e.is_list());
	CHECK_EQ(e.text(), std::string("`Keyboard/keyboard:X` | `RemoteGamePad/26770:Button 1`"));
	CHECK_EQ(e.leaves().size(), (size_t)2);
	CHECK(e.warnings().empty());

	// short device form, case-insensitive names, canonical output
	e = Expression::parse("`keyboard:x`|`SDLController:A`", dev);
	CHECK(e.ok());
	CHECK_EQ(e.text(), std::string("`Keyboard/keyboard:X` | `SDLController/0_030000005e040000:a`"));

	// #id and escaping
	e = Expression::parse("`Keyboard:#50`", dev);
	CHECK(e.ok());
	CHECK_EQ(e.text(), std::string("`Keyboard/keyboard:\\``"));
	auto e2 = Expression::parse(e.text(), dev);
	CHECK(e2.ok());
	CHECK(e2.leaves().size() == 1 && e2.leaves()[0].input == InputId(50));

	// precedence and parentheses
	e = Expression::parse("`Keyboard:A` & `Keyboard:B` | !`Keyboard:C`", dev);
	CHECK(e.ok());
	CHECK(!e.is_list());
	CHECK_EQ(e.text(), std::string("`Keyboard/keyboard:A` & `Keyboard/keyboard:B` | !`Keyboard/keyboard:C`"));
	e = Expression::parse("`Keyboard:A` & (`Keyboard:B` | `Keyboard:C`)", dev);
	CHECK_EQ(e.text(), std::string("`Keyboard/keyboard:A` & (`Keyboard/keyboard:B` | `Keyboard/keyboard:C`)"));
	e = Expression::parse("1 - (0.25 - 0.5)", dev);
	CHECK_EQ(e.text(), std::string("1 - (0.25 - 0.5)"));
	e = Expression::parse("deadzone(`SDLController:X-Axis+`, 0.2) * 2", dev);
	CHECK(e.ok());
	CHECK_EQ(e.text(), std::string("deadzone(`SDLController/0_030000005e040000:X-Axis+`, 0.2) * 2"));

	// empty
	e = Expression::parse("   ", dev);
	CHECK(e.ok());
	CHECK(e.empty());

	// errors keep the text
	for (const char* bad : {"`Keyboard:A` |", "`Keyboard:A", "foo(`Keyboard:A`)", "min(`Keyboard:A`)", "``", "`Keyboard:`", "(`Keyboard:A`", "`Keyboard:A` $ 1", "Keyboard"})
	{
		e = Expression::parse(bad, dev);
		CHECK(!e.ok());
		CHECK(!e.error().empty());
		CHECK(e.value(dev) == 0.0f);
	}
	e = Expression::parse("`Keyboard:A` |", dev);
	CHECK_EQ(e.text(), std::string("`Keyboard:A` |"));

	// unresolved inputs are kept as written, with warnings
	e = Expression::parse("`DSUController/0:Button 1` | `Keyboard:Nope` | `Keyboard:A`", dev);
	CHECK(e.ok());
	CHECK_EQ(e.warnings().size(), (size_t)2);
	CHECK_EQ(e.text(), std::string("`DSUController/0:Button 1` | `Keyboard/keyboard:Nope` | `Keyboard/keyboard:A`"));
}

static void test_eval()
{
	FakeDevices dev;
	auto e = Expression::parse("`Keyboard:X` | `RemoteGamePad:Button 1`", dev);
	CHECK(!e.active(dev));
	dev.set(0, 23, 1); // X
	CHECK(e.active(dev));
	dev.clear();
	dev.set(2, 1, 1);
	CHECK(e.active(dev));
	dev.clear();

	// sticks: the strongest
	e = Expression::parse("`SDLController:X-Axis+` | `Keyboard:D`", dev);
	dev.set(1, 40, 0.4f);
	CHECK(std::abs(e.value(dev) - 0.4f) < 1e-6);
	dev.set(0, 3, 1.0f);
	CHECK(std::abs(e.value(dev) - 1.0f) < 1e-6);
	dev.clear();

	// and / not / xor
	e = Expression::parse("`Keyboard:A` & !`Keyboard:B`", dev);
	dev.set(0, 0, 1);
	CHECK(e.active(dev));
	dev.set(0, 1, 1);
	CHECK(!e.active(dev));
	dev.clear();
	e = Expression::parse("`Keyboard:A` ^ `Keyboard:B`", dev);
	dev.set(0, 0, 1);
	CHECK(e.active(dev));
	dev.set(0, 1, 1);
	CHECK(!e.active(dev));
	dev.clear();

	// arithmetic and functions
	e = Expression::parse("`SDLController:X-Axis+` * 2", dev);
	dev.set(1, 40, 0.3f);
	CHECK(std::abs(e.value(dev) - 0.6f) < 1e-6);
	CHECK(e.active(dev));
	e = Expression::parse("clamp(`SDLController:X-Axis+` * 5, 0, 1)", dev);
	CHECK(std::abs(e.value(dev) - 1.0f) < 1e-6);
	e = Expression::parse("deadzone(`SDLController:X-Axis+`, 0.2)", dev);
	CHECK(std::abs(e.value(dev) - 0.125f) < 1e-5);
	e = Expression::parse("max(0.2, min(0.9, 0.5)) + abs(-1) / 4", dev);
	CHECK(std::abs(e.value(dev) - 0.75f) < 1e-6);
	CHECK(Expression::parse("1 / 0", dev).value(dev) == 0.0f);
	CHECK(Expression::parse("0.3 < 0.4", dev).active(dev));
	dev.clear();

	// toggle
	e = Expression::parse("toggle(`Keyboard:T`)", dev);
	CHECK(!e.active(dev));
	dev.set(0, 19, 1);
	CHECK(e.active(dev));
	CHECK(e.active(dev)); // still held: no new edge
	dev.clear();
	CHECK(e.active(dev));
	dev.set(0, 19, 1);
	CHECK(!e.active(dev));
	dev.clear();

	// hold / pulse
	e = Expression::parse("hold(`Keyboard:H`, 0.05)", dev);
	dev.set(0, 7, 1);
	CHECK(!e.active(dev));
	std::this_thread::sleep_for(std::chrono::milliseconds(70));
	CHECK(e.active(dev));
	dev.clear();
	CHECK(!e.active(dev));
	e = Expression::parse("pulse(`Keyboard:P`, 0.05)", dev);
	dev.set(0, 15, 1);
	CHECK(e.active(dev));
	dev.clear();
	CHECK(e.active(dev)); // released, still within the pulse
	std::this_thread::sleep_for(std::chrono::milliseconds(70));
	CHECK(!e.active(dev));
}

static void test_list_editing()
{
	FakeDevices dev;
	auto e = Expression::append(Expression{}, 0, 23, dev);
	CHECK_EQ(e.text(), std::string("`Keyboard/keyboard:X`"));
	e = Expression::append(e, 2, 1, dev);
	CHECK_EQ(e.text(), std::string("`Keyboard/keyboard:X` | `RemoteGamePad/26770:Button 1`"));
	e = Expression::append(e, 2, 1, dev); // no duplicate
	CHECK_EQ(e.leaves().size(), (size_t)2);
	CHECK(e.references_device(2));
	CHECK(!e.references_device(1));
	// no selected device: every input is written with its device and shown with it
	CHECK_EQ(e.display(dev), std::string("X [Keyboard] | Button 1 [Remote GamePad (UDP 26770)]"));
	CHECK(e.has_qualified_inputs());
	auto r = Expression::remove_device(e, 0, dev);
	CHECK_EQ(r.text(), std::string("`RemoteGamePad/26770:Button 1`"));
	r = Expression::remove_device(r, 2, dev);
	CHECK(r.empty());
	// expressions that aren't lists aren't edited by remove_device; append wraps them
	auto c = Expression::parse("`Keyboard:A` & `Keyboard:B`", dev);
	CHECK_EQ(Expression::remove_device(c, 0, dev).text(), c.text());
	CHECK_EQ(Expression::append(c, 1, 0, dev).text(), std::string("`Keyboard/keyboard:A` & `Keyboard/keyboard:B` | `SDLController/0_030000005e040000:a`"));
}

// an old profile (one input per control, in each device's <mappings>), as Cemu writes it
static const char* kOldProfile = R"(<?xml version="1.0" encoding="UTF-8"?>
<emulated_controller>
	<type>Wii U GamePad</type>
	<controller>
		<api>Keyboard</api>
		<uuid>keyboard</uuid>
		<display_name>Keyboard</display_name>
		<mappings>
			<entry><mapping>1</mapping><button>23</button></entry>
			<entry><mapping>2</mapping><button>24</button></entry>
		</mappings>
	</controller>
	<controller>
		<api>RemoteGamePad</api>
		<uuid>26770</uuid>
		<display_name>Remote GamePad (UDP 26770)</display_name>
		<mappings>
			<entry><mapping>1</mapping><button>1</button></entry>
			<entry><mapping>27</mapping><button>27</button></entry>
		</mappings>
	</controller>
</emulated_controller>
)";

static void test_profile()
{
	FakeDevices dev;
	pugi::xml_document doc;
	CHECK(doc.load_string(kOldProfile));
	auto root = doc.document_element();
	CHECK(!InputProfile::HasBindings(root));

	// load: each device's old entries become one input in the control's list (keyboard device 0,
	// Remote GamePad device 2 in the fake set)
	std::map<uint64_t, Expression> bindings;
	const size_t device_of_node[] = {0, 2};
	size_t n = 0;
	for (auto cnode : root.children("controller"))
	{
		for (const auto& [mapping, button] : InputProfile::ReadLegacyMappings(cnode))
			bindings[mapping] = Expression::append(bindings[mapping], device_of_node[n], button, dev);
		n++;
	}
	CHECK_EQ(bindings.size(), (size_t)3);
	CHECK_EQ(bindings[1].text(), std::string("`Keyboard/keyboard:X` | `RemoteGamePad/26770:Button 1`"));
	CHECK_EQ(bindings[2].text(), std::string("`Keyboard/keyboard:Y`"));
	CHECK_EQ(bindings[27].text(), std::string("`RemoteGamePad/26770:Button 27`"));

	// save in the new format (plus the old one for older builds) and load again
	std::map<uint64_t, std::string> texts;
	for (const auto& [m, e] : bindings)
		texts[m] = e.text();
	pugi::xml_document out;
	auto out_root = out.append_child("emulated_controller");
	auto kb = out_root.append_child("controller");
	InputProfile::WriteLegacyMappings(kb, {{1, 23}, {2, 24}});
	InputProfile::WriteBindings(out_root, texts);
	std::ostringstream xml;
	out.save(xml);

	pugi::xml_document in;
	CHECK(in.load_string(xml.str().c_str()));
	CHECK(InputProfile::HasBindings(in.document_element()));
	const auto reread = InputProfile::ReadBindings(in.document_element());
	CHECK(reread == texts);
	const auto legacy = InputProfile::ReadLegacyMappings(in.document_element().child("controller"));
	CHECK_EQ(legacy.size(), (size_t)2);
	for (const auto& [m, text] : reread)
	{
		const auto e = Expression::parse(text, dev);
		CHECK(e.ok());
		CHECK_EQ(e.text(), text);
	}
	// an expression with characters XML must escape survives
	texts[5] = "`Keyboard/keyboard:A` & !`Keyboard/keyboard:B` | 1 < 2";
	pugi::xml_document out2;
	auto r2 = out2.append_child("emulated_controller");
	InputProfile::WriteBindings(r2, texts);
	std::ostringstream xml2;
	out2.save(xml2);
	pugi::xml_document in2;
	CHECK(in2.load_string(xml2.str().c_str()));
	CHECK(InputProfile::ReadBindings(in2.document_element()) == texts);
}


// the keyboard and the Mouse device (its input names from MouseInputs, as MouseController has them)
struct MouseDevices : DeviceSet, InputSource
{
	std::map<std::pair<size_t, InputId>, float> values;
	size_t device_count() const override { return 2; }
	std::string device_api(size_t d) const override { return d == 0 ? "Keyboard" : "Mouse"; }
	std::string device_uuid(size_t d) const override { return d == 0 ? "keyboard" : "mouse"; }
	std::string device_display_name(size_t d) const override { return d == 0 ? "Keyboard" : "Mouse"; }
	std::string input_name(size_t d, InputId id) const override
	{
		if (d == 1)
			return MouseInputs::Name(id);
		return std::string(1, (char)('A' + id));
	}
	std::optional<InputId> find_input(size_t d, std::string_view name) const override
	{
		if (d == 1)
			return MouseInputs::FromName(name);
		if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z')
			return (InputId)(name[0] - 'A');
		return std::nullopt;
	}
	float value(size_t d, InputId id) const override
	{
		const auto it = values.find({d, id});
		return it == values.end() ? 0.0f : it->second;
	}
	// as MouseController: buttons/wheel pressed when set, cursor and motion past half way
	bool active(size_t d, InputId id) const override
	{
		const float v = value(d, id);
		return d == 1 && !MouseInputs::IsPressable(id) ? std::abs(v) > 0.5f : v > 0.1f;
	}
};

static void test_mouse_inputs()
{
	using namespace MouseInputs;
	// names: every input of every window maps back to its id, case-insensitively
	for (const auto id : AllIds())
	{
		CHECK(IsValid(id));
		CHECK(FromName(Name(id)) == std::optional<uint64_t>(id));
	}
	CHECK_EQ(AllIds().size(), (size_t)kWindowCount * (size_t)kInputCount);
	CHECK_EQ(Name(Id(kGamePad, kLeft)), std::string("GamePad Left"));
	CHECK_EQ(Name(Id(kTV, kCursorYN)), std::string("TV Cursor Y-"));
	CHECK_EQ(Name(Id(kAny, kWheelUp)), std::string("Wheel Up"));
	CHECK(FromName("gamepad cursor x+") == std::optional<uint64_t>(Id(kGamePad, kCursorXP)));
	CHECK(!FromName("Cursor Z").has_value());
	CHECK(IsPressable(Id(kTV, kRight)) && IsPressable(Id(kAny, kWheelDown)));
	CHECK(!IsPressable(Id(kTV, kCursorX)) && !IsPressable(Id(kAny, kMoveXP)));
	// ids stay below the expression code's name scan limit and the old profile format's range
	CHECK(AllIds().back() < 0x200);

	MouseDevices dev;
	// parse and print: the window prefix and the +/- are part of the name
	auto e = Expression::parse("`Mouse:GamePad Cursor X+` | `Mouse/mouse:wheel up`", dev);
	CHECK(e.ok());
	CHECK(e.warnings().empty());
	CHECK_EQ(e.text(), std::string("`Mouse/mouse:GamePad Cursor X+` | `Mouse/mouse:Wheel Up`"));
	CHECK_EQ(e.display(dev), std::string("GamePad Cursor X+ [Mouse] | Wheel Up [Mouse]"));
	CHECK(!Expression::parse("`Mouse:Cursor W`", dev).warnings().empty());

	// analogue values: a half axis gives the deflection, the full axis is signed
	dev.values[{1, Id(kGamePad, kCursorXP)}] = 0.4f;
	CHECK(std::abs(e.value(dev) - 0.4f) < 1e-5f);
	CHECK(!e.active(dev));
	dev.values[{1, Id(kGamePad, kCursorXP)}] = 0.8f;
	CHECK(e.active(dev));
	dev.values[{1, Id(kTV, kCursorX)}] = -0.75f;
	auto full = Expression::parse("`Mouse:TV Cursor X`", dev);
	CHECK(std::abs(full.value(dev) + 0.75f) < 1e-5f);
	// keyboard keys and the cursor combine like any inputs: a key steers, the mouse adds
	auto mixed = Expression::parse("`Keyboard:D` | `Mouse:TV Cursor X+` * 0.5", dev);
	CHECK(mixed.ok());
	dev.values[{1, Id(kTV, kCursorXP)}] = 1.0f;
	CHECK(std::abs(mixed.value(dev) - 0.5f) < 1e-5f);
	dev.values[{0, 3}] = 1.0f;
	CHECK(std::abs(mixed.value(dev) - 1.0f) < 1e-5f);
	// relative motion only while a button is held
	auto drag = Expression::parse("`Mouse:Move X+` & `Mouse:Right`", dev);
	dev.values[{1, Id(kAny, kMoveXP)}] = 0.6f;
	CHECK(!drag.active(dev));
	CHECK(drag.value(dev) == 0.0f);
	dev.values[{1, Id(kAny, kRight)}] = 1.0f;
	CHECK(std::abs(drag.value(dev) - 0.6f) < 1e-5f);
	// clicks and the wheel are buttons
	auto click = Expression::parse("`Mouse:GamePad Left`", dev);
	CHECK(!click.active(dev));
	dev.values[{1, Id(kGamePad, kLeft)}] = 1.0f;
	CHECK(click.active(dev));
}

// a GamePad profile written by the input rework before the mouse groups: <bindings> without a
// version, keyboard buttons, nothing for touch or motion
static const char* kReworkProfile = R"(<?xml version="1.0" encoding="UTF-8"?>
<emulated_controller>
	<type>Wii U GamePad</type>
	<controller><api>Keyboard</api><uuid>keyboard</uuid><display_name>Keyboard</display_name></controller>
	<bindings>
		<binding id="1">`Keyboard/keyboard:X`</binding>
	</bindings>
</emulated_controller>
)";

static void test_mouse_conversion()
{
	using namespace MouseBindings;
	MouseDevices dev;

	// version: old format 0, rework 1, explicit mouse 2
	{
		pugi::xml_document old_doc, rework;
		CHECK(old_doc.load_string(kOldProfile));
		CHECK(rework.load_string(kReworkProfile));
		CHECK_EQ(InputProfile::BindingsVersion(old_doc.document_element()), 0);
		CHECK_EQ(InputProfile::BindingsVersion(rework.document_element()), 1);
		pugi::xml_document out;
		auto root = out.append_child("emulated_controller");
		InputProfile::WriteBindings(root, {{1, "`Keyboard/keyboard:X`"}});
		CHECK_EQ(InputProfile::BindingsVersion(root), kBindingsVersion);
	}

	// GamePad: the implicit touch and right-drag tilt become Mouse bindings
	pugi::xml_document doc;
	CHECK(doc.load_string(kReworkProfile));
	auto bindings = InputProfile::ReadBindings(doc.document_element());
	auto added = ImplicitMouseBindings(kTypeGamePad, bindings, false);
	CHECK_EQ(added.size(), (size_t)12);
	CHECK_EQ(added[kTouchPress], std::string("`Mouse/mouse:GamePad Left`"));
	CHECK_EQ(added[kTouchLeft], std::string("`Mouse/mouse:GamePad Cursor X-`"));
	CHECK_EQ(added[kTouchDown], std::string("`Mouse/mouse:GamePad Cursor Y+`"));
	CHECK_EQ(added[kTiltHold], std::string("`Mouse/mouse:Right`"));
	CHECK_EQ(added[kTiltUp], std::string("`Mouse/mouse:Cursor Y-`"));
	CHECK_EQ(added[kTurnLeft], std::string("`Mouse/mouse:Wheel Up`"));
	CHECK(!added.contains(1));
	// they parse against a device list with the mouse, with no warnings
	for (const auto& [control, text] : added)
	{
		const auto e = Expression::parse(text, dev);
		CHECK(e.ok());
		CHECK(e.warnings().empty());
		CHECK_EQ(e.text(), text);
	}
	// what they do: touching with the left button in the GamePad window, at the cursor
	{
		const auto press = Expression::parse(added[kTouchPress], dev);
		const auto right = Expression::parse(added[kTouchRight], dev);
		dev.values[{1, MouseInputs::Id(MouseInputs::kGamePad, MouseInputs::kCursorXP)}] = 0.5f;
		CHECK(!press.active(dev));
		dev.values[{1, MouseInputs::Id(MouseInputs::kGamePad, MouseInputs::kLeft)}] = 1.0f;
		CHECK(press.active(dev));
		CHECK(std::abs(right.value(dev) - 0.5f) < 1e-5f);
	}

	// a group that is already bound (by the user) is left alone; the other one is still added
	bindings[kTouchPress] = "`Keyboard/keyboard:T`";
	added = ImplicitMouseBindings(kTypeGamePad, bindings, false);
	CHECK_EQ(added.size(), (size_t)7);
	CHECK(!added.contains(kTouchPress) && added.contains(kTiltHold));

	// Balance Board: the cursor over the TV is the centre of balance, unless the balance is bound or
	// a real board (Wiimote API device) is assigned
	added = ImplicitMouseBindings(kTypeBoard, {{1, "`Keyboard/keyboard:P`"}}, false);
	CHECK_EQ(added.size(), (size_t)4);
	CHECK_EQ(added[kBalanceUp], std::string("`Mouse/mouse:TV Cursor Y-`"));
	CHECK_EQ(added[kBalanceRight], std::string("`Mouse/mouse:TV Cursor X+`"));
	CHECK(ImplicitMouseBindings(kTypeBoard, {}, true).empty());
	CHECK(ImplicitMouseBindings(kTypeBoard, {{kBalanceLeft, "`Keyboard/keyboard:A`"}}, false).empty());

	// the mouse did nothing for the Wii Remote, Pro and Classic controllers
	CHECK(ImplicitMouseBindings(kTypeWiimote, {}, false).empty());
	CHECK(ImplicitMouseBindings("Wii U Pro Controller", {}, false).empty());

	// the Mouse device's defaults: touch + tilt on the GamePad, pointer + A/B on the Wii Remote
	CHECK_EQ(MouseDefaults(kTypeGamePad).size(), (size_t)12);
	CHECK_EQ(MouseDefaults(kTypeWiimote).size(), (size_t)6);
	CHECK_EQ(MouseDefaults(kTypeBoard).size(), (size_t)4);

	// after conversion the profile is saved as version 2 and isn't converted again
	pugi::xml_document out;
	auto root = out.append_child("emulated_controller");
	InputProfile::WriteBindings(root, bindings);
	std::ostringstream xml;
	out.save(xml);
	pugi::xml_document in;
	CHECK(in.load_string(xml.str().c_str()));
	CHECK(InputProfile::BindingsVersion(in.document_element()) >= kBindingsVersion);
}

// one selected device per emulated controller: unqualified inputs are that device's
static void test_default_device()
{
	FakeDevices dev;
	dev.def = 2; // the Remote GamePad is the controller's device
	auto e = Expression::parse("`Button 1` | `Keyboard/keyboard:X`", dev);
	CHECK(e.ok());
	CHECK(e.warnings().empty());
	CHECK_EQ(e.leaves().size(), (size_t)2);
	CHECK(!e.leaves()[0].qualified && e.leaves()[0].device == 2 && e.leaves()[0].input == InputId(1));
	CHECK(e.leaves()[1].qualified && e.leaves()[1].device == 0 && e.leaves()[1].input == InputId(23));
	CHECK_EQ(e.text(), std::string("`Button 1` | `Keyboard/keyboard:X`"));
	CHECK_EQ(e.display(dev), std::string("Button 1 | X [Keyboard]"));
	CHECK(e.has_qualified_inputs());
	CHECK(!Expression::parse("`Button 1` | `button 2`", dev).has_qualified_inputs());
	CHECK_EQ(Expression::parse("`button 2`", dev).text(), std::string("`Button 2`"));

	// a qualifier naming the selected device stays as written (explicit)
	e = Expression::parse("`RemoteGamePad:Button 2`", dev);
	CHECK_EQ(e.text(), std::string("`RemoteGamePad/26770:Button 2`"));
	CHECK_EQ(e.display(dev), std::string("Button 2 [Remote GamePad (UDP 26770)]"));

	// evaluation: unqualified inputs read the selected device, qualified ones their own device
	e = Expression::parse("`Button 1` | `Keyboard/keyboard:X`", dev);
	CHECK(!e.active(dev));
	dev.set(2, 1, 1);
	CHECK(e.active(dev));
	dev.clear();
	dev.set(0, 23, 1);
	CHECK(e.active(dev));
	dev.clear();
	dev.set(0, 1, 1); // the keyboard's "B": not this binding's Button 1
	CHECK(!e.active(dev));
	dev.clear();

	// the same text against another selected device: `X` follows the device (Dolphin's model)
	dev.def = 0;
	e = Expression::parse("`X`", dev);
	CHECK(e.warnings().empty() && e.leaves()[0].device == 0 && e.leaves()[0].input == InputId(23));
	dev.def = 2;
	e = Expression::parse("`X`", dev);
	CHECK_EQ(e.warnings().size(), (size_t)1); // the Remote GamePad has no X
	CHECK_EQ(e.display(dev), std::string("X (?)"));
	CHECK_EQ(e.text(), std::string("`X`"));
	dev.def = kNoDevice;
	e = Expression::parse("`X`", dev);
	CHECK_EQ(e.warnings().size(), (size_t)1);
	CHECK(e.warnings()[0].find("no device selected") != std::string::npos);

	// #id on the selected device prints as its name
	dev.def = 0;
	CHECK_EQ(Expression::parse("`#15`", dev).text(), std::string("`P`"));

	// a text before ':' that isn't a device name is part of the input name
	e = Expression::parse("`1:2`", dev);
	CHECK(e.ok() && !e.leaves()[0].qualified);
	CHECK_EQ(e.leaves()[0].input_text, std::string("1:2"));
	CHECK(looks_like_device("Keyboard") && looks_like_device("RemoteGamePad/26770") && looks_like_device("SDLController/0_03ab"));
	CHECK(!looks_like_device("1") && !looks_like_device("Key board") && !looks_like_device("Keyboard/") && !looks_like_device(""));

	// list editing writes the selected device's inputs without a device, others with one
	dev.def = 2;
	auto l = Expression::append(Expression{}, 2, 3, dev);
	CHECK_EQ(l.text(), std::string("`Button 3`"));
	l = Expression::append(l, 0, 23, dev);
	CHECK_EQ(l.text(), std::string("`Button 3` | `Keyboard/keyboard:X`"));
	CHECK_EQ(Expression::from_input(2, 4, dev).text(), std::string("`Button 4`"));
	CHECK_EQ(Expression::remove_device(l, 0, dev).text(), std::string("`Button 3`"));
	size_t dropped = 0;
	CHECK(Expression::remove_leaves(l, [](const Leaf&) { return true; }, dev, &dropped).empty());
	CHECK_EQ(dropped, (size_t)2);

	// operators and functions work the same with unqualified inputs
	e = Expression::parse("toggle(`Button 5`) & !`Keyboard/keyboard:Space`", dev);
	CHECK(e.ok());
	CHECK_EQ(e.text(), std::string("toggle(`Button 5`) & !`Keyboard/keyboard:Space`"));
}

static void test_keyboard_mouse_names()
{
	using namespace MouseInputs;
	for (const auto id : AllIds())
	{
		const auto kid = KeyboardId(id);
		CHECK(IsKeyboardMouseId(kid));
		CHECK(MouseIdOf(kid) == id);
		CHECK(FromCombinedName(CombinedName(id)) == std::optional<uint64_t>(kid));
	}
	CHECK_EQ(CombinedName(Id(kGamePad, kLeft)), std::string("GamePad Mouse Left"));
	CHECK_EQ(CombinedName(Id(kAny, kRight)), std::string("Mouse Right"));
	CHECK_EQ(CombinedName(Id(kTV, kCursorYN)), std::string("TV Cursor Y-"));
	CHECK_EQ(CombinedName(Id(kAny, kWheelUp)), std::string("Wheel Up"));
	// key names don't collide: no "Left"/"Right" for the mouse buttons (arrow keys have those)
	CHECK(!FromCombinedName("Left").has_value() && !FromCombinedName("Right").has_value());
	CHECK(FromCombinedName("gamepad mouse left") == std::optional<uint64_t>(KeyboardId(Id(kGamePad, kLeft))));
	// key codes stay below the mouse range (GDK key values < 0x11000000)
	CHECK(!IsKeyboardMouseId(0x1008FFFF) && !IsKeyboardMouseId(49));
	CHECK_EQ(MouseBindings::CombinedInputText(Id(kGamePad, kLeft)), std::string("`GamePad Mouse Left`"));
	CHECK_EQ(MouseBindings::QualifiedMouseText(Id(kTV, kCursorXP)), std::string("`Keyboard/keyboard:TV Cursor X+`"));
}

using SingleDeviceMigration::ProfileDevice;

static std::map<uint64_t, std::string> with_implicit_mouse(std::string_view type, std::map<uint64_t, std::string> bindings, bool has_wii_device, std::vector<ProfileDevice>& devices)
{
	const auto added = MouseBindings::ImplicitMouseBindings(type, bindings, has_wii_device);
	if (!added.empty())
		devices.push_back({"Mouse", "mouse", "Mouse"});
	for (const auto& [id, text] : added)
		bindings[id] = text;
	return bindings;
}

static void test_migration()
{
	using namespace SingleDeviceMigration;
	using namespace MouseBindings;
	const ProfileDevice keyboard{"Keyboard", "keyboard", "Keyboard"};
	const ProfileDevice remote{"RemoteGamePad", "26770", "Remote GamePad (UDP 26770)"};
	const ProfileDevice wiimote{"Wiimote", "0", "Controller 1"};
	const ProfileDevice sdl{"SDLController", "0_030000005e040000", "Xbox Controller"};
	const ProfileDevice dsu{"DSUController", "0", "Controller 1"};

	// the user's controller0: Keyboard (no bindings) + Remote GamePad (version 1 bindings) + the
	// mouse's implicit touch and tilt -> the Remote GamePad, the mouse's 12 bindings dropped
	{
		std::vector<ProfileDevice> devices{keyboard, remote};
		std::map<uint64_t, std::string> bindings{{1, "`RemoteGamePad/26770:A`"}, {17, "`RemoteGamePad/26770:Y-Axis+`"}, {27, "`RemoteGamePad/26770:Home`"}};
		bindings = with_implicit_mouse(kTypeGamePad, bindings, false, devices);
		CHECK_EQ(devices.size(), (size_t)3);
		CHECK_EQ(bindings.size(), (size_t)15);
		CHECK(ChooseDevice(devices, bindings) == std::optional<size_t>(1));
		const auto r = Migrate(devices, bindings);
		CHECK(r.device.has_value() && r.device->api == "RemoteGamePad" && r.device->uuid == "26770");
		CHECK_EQ(r.bindings.size(), (size_t)3);
		CHECK_EQ(r.bindings.at(1), std::string("`A`"));
		CHECK_EQ(r.bindings.at(17), std::string("`Y-Axis+`"));
		CHECK(!r.bindings.contains(kTouchPress) && !r.bindings.contains(kTiltHold));
		CHECK_EQ(r.dropped.size(), (size_t)1);
		CHECK_EQ(r.dropped[0].device, std::string("Keyboard/Mouse"));
		CHECK_EQ(r.dropped[0].inputs, (size_t)12);
		CHECK_EQ(r.dropped_inputs(), (size_t)12);
		CHECK_EQ(Note(r), std::string("Bindings from Keyboard/Mouse (12) were removed: one device per controller."));
		// the result loads against the real devices without warnings
		FakeDevices dev;
		dev.devs[2].inputs = {{0, "A"}, {10, "Home"}, {40, "Y-Axis+"}};
		dev.def = 2;
		for (const auto& [id, text] : r.bindings)
		{
			const auto e = Expression::parse(text, dev);
			CHECK(e.ok() && e.warnings().empty() && !e.has_qualified_inputs());
		}
	}

	// a Wii Remote controller with Keyboard + a real Wii Remote: the Wii Remote, Space dropped
	{
		const std::vector<ProfileDevice> devices{keyboard, wiimote};
		const std::map<uint64_t, std::string> bindings{{17, "`Keyboard/keyboard:Space`"}, {1, "`Wiimote/0:A` | `Keyboard/keyboard:X`"}};
		const auto r = Migrate(devices, bindings);
		CHECK(r.device && r.device->api == "Wiimote");
		CHECK_EQ(r.bindings.size(), (size_t)1);
		CHECK_EQ(r.bindings.at(1), std::string("`A`"));
		CHECK_EQ(r.dropped_inputs(), (size_t)2);
		CHECK_EQ(Note(r), std::string("Bindings from Keyboard/Mouse (2) were removed: one device per controller."));
	}

	// the Balance Board with Keyboard (P, 6) + a Wii Remote (the white-screen case): the Wii device
	// is kept (the board panel then warns that it isn't a board), the keys are dropped
	{
		std::vector<ProfileDevice> devices{keyboard, wiimote};
		std::map<uint64_t, std::string> bindings{{1, "`Keyboard/keyboard:P` | `Wiimote/0:A`"}, {6, "`Keyboard/keyboard:6`"}};
		bindings = with_implicit_mouse(kTypeBoard, bindings, true, devices); // a Wii device: no mouse balance
		CHECK_EQ(devices.size(), (size_t)2);
		const auto r = Migrate(devices, bindings);
		CHECK(r.device && r.device->api == "Wiimote" && r.device->uuid == "0");
		CHECK_EQ(r.bindings.size(), (size_t)1);
		CHECK_EQ(r.bindings.at(1), std::string("`A`"));
		CHECK_EQ(r.dropped_inputs(), (size_t)2);
	}

	// single device profiles convert 1:1: the keyboard alone, and the keyboard + the mouse (one
	// device now: the mouse's names change, nothing is dropped)
	{
		std::vector<ProfileDevice> devices{keyboard};
		std::map<uint64_t, std::string> bindings{{1, "`Keyboard/keyboard:P`"}, {6, "`Keyboard/keyboard:#22`"}};
		bindings = with_implicit_mouse(kTypeBoard, bindings, false, devices);
		CHECK_EQ(devices.size(), (size_t)2);
		const auto r = Migrate(devices, bindings);
		CHECK(r.device && r.device->api == "Keyboard" && r.device->display_name == "Keyboard/Mouse");
		CHECK_EQ(r.bindings.size(), (size_t)6);
		CHECK_EQ(r.bindings.at(1), std::string("`P`"));
		CHECK_EQ(r.bindings.at(6), std::string("`#22`"));
		CHECK_EQ(r.bindings.at(kBalanceUp), std::string("`TV Cursor Y-`"));
		CHECK(r.dropped.empty());
		CHECK(Note(r).empty());
		// `#22` becomes the key's name against the real keyboard
		FakeDevices dev;
		dev.def = 0;
		CHECK_EQ(Expression::parse(r.bindings.at(6), dev).text(), std::string("`W`"));
	}
	{
		const std::vector<ProfileDevice> devices{remote};
		const auto r = Migrate(devices, {{1, "`RemoteGamePad/26770:A`"}});
		CHECK(r.device && r.device->api == "RemoteGamePad");
		CHECK_EQ(r.bindings.at(1), std::string("`A`"));
		CHECK(r.dropped.empty());
	}
	// a version 2 profile with the separate Mouse device: GamePad window clicks become Keyboard/Mouse
	// inputs; the mouse alone is Keyboard/Mouse too
	{
		const std::vector<ProfileDevice> devices{{"Mouse", "mouse", "Mouse"}};
		const auto r = Migrate(devices, {{kTouchPress, "`Mouse/mouse:GamePad Left`"}, {kTiltHold, "`Mouse/mouse:#1`"}});
		CHECK(r.device && r.device->api == "Keyboard" && r.device->uuid == "keyboard");
		CHECK_EQ(r.bindings.at(kTouchPress), std::string("`GamePad Mouse Left`"));
		CHECK_EQ(r.bindings.at(kTiltHold), std::string("`Mouse Right`"));
	}

	// the main device: the non-keyboard device with the most inputs, then Remote GamePad > Wii > others
	{
		const std::vector<ProfileDevice> devices{keyboard, sdl, dsu};
		const std::map<uint64_t, std::string> bindings{{1, "`Keyboard/keyboard:X` | `DSUController/0:a`"}, {2, "`Keyboard/keyboard:Z`"}};
		CHECK(ChooseDevice(devices, bindings) == std::optional<size_t>(2));
		CHECK(ChooseDevice({keyboard, sdl, remote}, {}) == std::optional<size_t>(2));
		CHECK(ChooseDevice({keyboard, sdl}, {{1, "`Keyboard/keyboard:X`"}}) == std::optional<size_t>(1)); // a pad without bindings still wins over the keyboard
		CHECK(ChooseDevice({keyboard, {"Mouse", "mouse", "Mouse"}}, {}) == std::optional<size_t>(0));
		CHECK(!ChooseDevice({}, {}).has_value());
		const auto r = Migrate(devices, bindings);
		CHECK(r.device && r.device->api == "DSUController");
		CHECK_EQ(r.bindings.at(1), std::string("`a`"));
		CHECK(!r.bindings.contains(2));
		CHECK_EQ(r.dropped.size(), (size_t)2); // Keyboard/Mouse (2), the SDL pad (0)
		CHECK_EQ(Note(r), std::string("Bindings from Keyboard/Mouse (2) were removed: one device per controller."));
	}

	// advanced expressions are kept whole: other devices stay explicit, the mouse becomes Keyboard/Mouse
	{
		const std::vector<ProfileDevice> devices{keyboard, remote, {"Mouse", "mouse", "Mouse"}};
		const std::map<uint64_t, std::string> bindings{
			{1, "`Keyboard/keyboard:A` & `RemoteGamePad/26770:B`"},
			{kTouchPress, "`Mouse/mouse:GamePad Left` & !`RemoteGamePad/26770:ZL`"},
			{2, "toggle(`RemoteGamePad/26770:X`)"},
			{3, "`Keyboard/keyboard:A` |"}, // doesn't parse: kept as text
		};
		const auto r = Migrate(devices, bindings);
		CHECK(r.device && r.device->api == "RemoteGamePad");
		CHECK_EQ(r.bindings.at(1), std::string("`Keyboard/keyboard:A` & `B`"));
		CHECK_EQ(r.bindings.at(kTouchPress), std::string("`Keyboard/keyboard:GamePad Mouse Left` & !`ZL`"));
		CHECK_EQ(r.bindings.at(2), std::string("toggle(`X`)"));
		CHECK_EQ(r.bindings.at(3), std::string("`Keyboard/keyboard:A` |"));
		CHECK_EQ(r.advanced_kept, (size_t)2);
		CHECK_EQ(r.dropped_inputs(), (size_t)0);
		CHECK(Note(r).empty());
	}

	// inputs of a device that isn't in the profile are dropped from lists too
	{
		const auto r = Migrate({remote}, {{1, "`RemoteGamePad/26770:A` | `SDLController/9_ff:b`"}});
		CHECK_EQ(r.bindings.at(1), std::string("`A`"));
		CHECK_EQ(r.dropped_inputs(), (size_t)1);
	}
}

int main()
{
	test_parse_and_print();
	test_eval();
	test_list_editing();
	test_profile();
	test_mouse_inputs();
	test_mouse_conversion();
	test_default_device();
	test_keyboard_mouse_names();
	test_migration();
	if (g_failures)
	{
		std::printf("%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("all input expression tests passed\n");
	return 0;
}
