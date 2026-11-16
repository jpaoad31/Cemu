#include "input/emulated/InputProfileBindings.h"

#include <pugixml.hpp>

#include <cstdlib>
#include <string_view>

namespace InputProfile
{
	namespace
	{
		bool parse_u64(std::string_view text, uint64_t& out)
		{
			while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\n' || text.front() == '\r'))
				text.remove_prefix(1);
			while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\n' || text.back() == '\r'))
				text.remove_suffix(1);
			if (text.empty())
				return false;
			uint64_t v = 0;
			for (char c : text)
			{
				if (c < '0' || c > '9')
					return false;
				v = v * 10 + (uint64_t)(c - '0');
			}
			out = v;
			return true;
		}
	}

	bool HasBindings(const pugi::xml_node& root)
	{
		return (bool)root.child("bindings");
	}

	std::map<uint64_t, std::string> ReadBindings(const pugi::xml_node& root)
	{
		std::map<uint64_t, std::string> result;
		for (const auto binding : root.child("bindings").children("binding"))
		{
			uint64_t id;
			if (!parse_u64(binding.attribute("id").value(), id))
				continue;
			std::string text = binding.child_value();
			if (!text.empty())
				result[id] = std::move(text);
		}
		return result;
	}

	int BindingsVersion(const pugi::xml_node& root)
	{
		const auto node = root.child("bindings");
		if (!node)
			return 0;
		return node.attribute("version").as_int(1);
	}

	void WriteBindings(pugi::xml_node& root, const std::map<uint64_t, std::string>& bindings, int version)
	{
		auto node = root.append_child("bindings");
		node.append_attribute("version") = version;
		for (const auto& [id, text] : bindings)
		{
			if (text.empty())
				continue;
			auto b = node.append_child("binding");
			b.append_attribute("id") = (unsigned long long)id;
			b.append_child(pugi::node_pcdata).set_value(text.c_str());
		}
	}

	std::vector<std::pair<uint64_t, uint64_t>> ReadLegacyMappings(const pugi::xml_node& controller_node)
	{
		std::vector<std::pair<uint64_t, uint64_t>> result;
		for (const auto entry : controller_node.child("mappings").children("entry"))
		{
			uint64_t mapping, button;
			if (parse_u64(entry.child_value("mapping"), mapping) && parse_u64(entry.child_value("button"), button))
				result.emplace_back(mapping, button);
		}
		return result;
	}

	void WriteLegacyMappings(pugi::xml_node& controller_node, const std::vector<std::pair<uint64_t, uint64_t>>& mappings)
	{
		auto node = controller_node.append_child("mappings");
		for (const auto& [mapping, button] : mappings)
		{
			auto entry = node.append_child("entry");
			entry.append_child("mapping").append_child(pugi::node_pcdata).set_value(std::to_string(mapping).c_str());
			entry.append_child("button").append_child(pugi::node_pcdata).set_value(std::to_string(button).c_str());
		}
	}
}
