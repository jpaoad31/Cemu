// Standalone test of the binding expressions and the profile bindings (old -> new format).
// Not in CMake (Cemu has no test target). Build and run:
//
//   P=build/vcpkg_installed/arm64-osx
//   clang++ -std=c++20 -Wall -I src -I $P/include src/input/emulated/InputExpressionTest.cpp \
//     src/input/emulated/InputExpression.cpp src/input/emulated/InputProfileBindings.cpp \
//     $P/lib/libpugixml.a -o /tmp/exprtest && /tmp/exprtest

#include "input/emulated/InputExpression.h"
#include "input/emulated/InputProfileBindings.h"

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
	for (const char* bad : {"`Keyboard:A` |", "`Keyboard:A", "foo(`Keyboard:A`)", "min(`Keyboard:A`)", "`A`", "(`Keyboard:A`", "`Keyboard:A` $ 1", "Keyboard"})
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
	CHECK_EQ(e.display(dev, false), std::string("X | Button 1"));
	CHECK_EQ(e.display(dev, true), std::string("X [Keyboard] | Button 1 [Remote GamePad (UDP 26770)]"));
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

int main()
{
	test_parse_and_print();
	test_eval();
	test_list_editing();
	test_profile();
	if (g_failures)
	{
		std::printf("%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("all input expression tests passed\n");
	return 0;
}
