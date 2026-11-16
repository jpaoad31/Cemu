#pragma once

// Bindings in the controller profile XML (fork addition, see INPUT_REWORK.md).
//
// New format: one expression per emulated control, after the <controller> node of the selected
// device (version 3: one device per emulated controller; `X` is the selected device's X):
//   <bindings version="3">
//     <binding id="1">`X` | `RemoteGamePad/26770:A`</binding>
//   </bindings>
// Old format (still read when <bindings> is missing, and still written so older Cemu builds load
// the profile): each <controller> node has <mappings><entry><mapping>N</mapping><button>B</button>.
//
// pugixml only, so the tests can use it without Cemu.

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace pugi
{
	class xml_node;
}

namespace InputProfile
{
	bool HasBindings(const pugi::xml_node& root);
	std::map<uint64_t, std::string> ReadBindings(const pugi::xml_node& root);
	// 0: no <bindings> (old format), 1: <bindings> without a version (the mouse was still implicit),
	// 2: <bindings version="2"> (mouse use is explicit, see MouseBindings.h), 3: one device per
	// emulated controller (see SingleDeviceMigration.h)
	int BindingsVersion(const pugi::xml_node& root);
	void WriteBindings(pugi::xml_node& root, const std::map<uint64_t, std::string>& bindings, int version = 3);

	std::vector<std::pair<uint64_t, uint64_t>> ReadLegacyMappings(const pugi::xml_node& controller_node);
	void WriteLegacyMappings(pugi::xml_node& controller_node, const std::vector<std::pair<uint64_t, uint64_t>>& mappings);
}
