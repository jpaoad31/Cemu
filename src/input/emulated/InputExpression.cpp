// Input binding expressions (fork addition). Design after Dolphin's ControlReference expressions;
// our own grammar and code. See InputExpression.h and INPUT_REWORK.md.

#include "input/emulated/InputExpression.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>

namespace InputExpr
{
	using Clock = std::chrono::steady_clock;

	struct Node
	{
		enum Kind
		{
			kLeaf,
			kNumber,
			kNot,
			kNeg,
			kBinary,
			kFunc,
		};
		Kind kind = kNumber;
		char op = 0; // binary operator
		double number = 0;
		Leaf leaf;
		std::string func;
		std::vector<std::shared_ptr<Node>> args; // operands / arguments

		// state of toggle/hold/pulse (evaluated from the emulation and the UI threads)
		mutable std::mutex state_mutex;
		mutable bool last_active = false;
		mutable bool state = false;
		mutable Clock::time_point since{};
	};

	namespace
	{
		bool iequals(std::string_view a, std::string_view b)
		{
			if (a.size() != b.size())
				return false;
			for (size_t i = 0; i < a.size(); i++)
			{
				if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
					return false;
			}
			return true;
		}

		std::string_view trim(std::string_view s)
		{
			while (!s.empty() && std::isspace((unsigned char)s.front()))
				s.remove_prefix(1);
			while (!s.empty() && std::isspace((unsigned char)s.back()))
				s.remove_suffix(1);
			return s;
		}

		std::string escape(std::string_view s)
		{
			std::string out;
			for (char c : s)
			{
				if (c == '`' || c == '\\')
					out += '\\';
				out += c;
			}
			return out;
		}

		std::string format_number(double v)
		{
			char buf[64];
			std::snprintf(buf, sizeof(buf), "%g", v);
			return buf;
		}

		// ---- resolution ----

		size_t find_device(const DeviceSet& devices, std::string_view device_text)
		{
			device_text = trim(device_text);
			std::string_view api = device_text, uuid;
			if (const auto slash = device_text.find('/'); slash != std::string_view::npos)
			{
				api = device_text.substr(0, slash);
				uuid = device_text.substr(slash + 1);
			}
			for (size_t i = 0; i < devices.device_count(); i++)
			{
				if (!iequals(devices.device_api(i), api))
					continue;
				if (uuid.empty() || devices.device_uuid(i) == uuid)
					return i;
			}
			return kNoDevice;
		}

		std::optional<InputId> find_input(const DeviceSet& devices, size_t dev, std::string_view input_text)
		{
			input_text = trim(input_text);
			if (input_text.size() > 1 && input_text[0] == '#')
			{
				InputId id = 0;
				for (char c : input_text.substr(1))
				{
					if (c < '0' || c > '9')
						return std::nullopt;
					id = id * 10 + (InputId)(c - '0');
				}
				return id;
			}
			if (dev == kNoDevice)
				return std::nullopt;
			return devices.find_input(dev, input_text);
		}

		void resolve(Leaf& leaf, const DeviceSet& devices)
		{
			leaf.device = find_device(devices, leaf.device_text);
			leaf.input = leaf.device == kNoDevice ? std::nullopt : find_input(devices, leaf.device, leaf.input_text);
		}

		// ---- tokenizer / parser ----

		struct Token
		{
			enum Type
			{
				kEnd,
				kInput,
				kNumber,
				kIdent,
				kOp,
			};
			Type type = kEnd;
			std::string text;
			double number = 0;
			size_t pos = 0;
		};

		class Parser
		{
		public:
			Parser(std::string_view text, const DeviceSet& devices) : m_text(text), m_devices(devices) {}

			std::shared_ptr<Node> parse(std::string& error)
			{
				if (!tokenize(error))
					return {};
				if (m_tokens.size() == 1) // only the end token
					return {};
				auto root = parse_or();
				if (root && peek().type != Token::kEnd)
					fail(fmt("unexpected '{}'", peek().text), peek().pos);
				if (!m_error.empty())
				{
					error = m_error;
					return {};
				}
				return root;
			}

		private:
			static std::string fmt(std::string_view pattern, std::string_view arg)
			{
				std::string out{pattern};
				if (const auto p = out.find("{}"); p != std::string::npos)
					out.replace(p, 2, arg);
				return out;
			}

			void fail(const std::string& message, size_t pos)
			{
				if (m_error.empty())
					m_error = message + " (at " + std::to_string(pos + 1) + ")";
			}

			bool tokenize(std::string& error)
			{
				size_t i = 0;
				while (i < m_text.size())
				{
					const char c = m_text[i];
					if (std::isspace((unsigned char)c))
					{
						i++;
						continue;
					}
					Token t;
					t.pos = i;
					if (c == '`')
					{
						std::string content;
						size_t j = i + 1;
						bool closed = false;
						while (j < m_text.size())
						{
							if (m_text[j] == '\\' && j + 1 < m_text.size())
							{
								content += m_text[j + 1];
								j += 2;
								continue;
							}
							if (m_text[j] == '`')
							{
								closed = true;
								break;
							}
							content += m_text[j++];
						}
						if (!closed)
						{
							error = "missing closing ` (at " + std::to_string(i + 1) + ")";
							return false;
						}
						t.type = Token::kInput;
						t.text = content;
						i = j + 1;
					}
					else if (std::isdigit((unsigned char)c) || (c == '.' && i + 1 < m_text.size() && std::isdigit((unsigned char)m_text[i + 1])))
					{
						size_t j = i;
						while (j < m_text.size() && (std::isdigit((unsigned char)m_text[j]) || m_text[j] == '.'))
							j++;
						t.type = Token::kNumber;
						t.text = std::string(m_text.substr(i, j - i));
						try
						{
							t.number = std::stod(t.text);
						}
						catch (...)
						{
							error = "bad number " + t.text;
							return false;
						}
						i = j;
					}
					else if (std::isalpha((unsigned char)c) || c == '_')
					{
						size_t j = i;
						while (j < m_text.size() && (std::isalnum((unsigned char)m_text[j]) || m_text[j] == '_'))
							j++;
						t.type = Token::kIdent;
						t.text = std::string(m_text.substr(i, j - i));
						i = j;
					}
					else if (std::string_view("|&^!+-*/<>(),").find(c) != std::string_view::npos)
					{
						t.type = Token::kOp;
						t.text = std::string(1, c);
						i++;
					}
					else
					{
						error = std::string("unexpected character '") + c + "' (at " + std::to_string(i + 1) + ")";
						return false;
					}
					m_tokens.emplace_back(std::move(t));
				}
				Token end;
				end.pos = m_text.size();
				m_tokens.emplace_back(end);
				return true;
			}

			const Token& peek() const { return m_tokens[m_index]; }
			const Token& next() { return m_tokens[std::min(m_index++, m_tokens.size() - 1)]; }
			bool is_op(std::string_view op) const { return peek().type == Token::kOp && peek().text == op; }

			std::shared_ptr<Node> binary(char op, std::shared_ptr<Node> a, std::shared_ptr<Node> b)
			{
				if (!a || !b)
					return {};
				auto n = std::make_shared<Node>();
				n->kind = Node::kBinary;
				n->op = op;
				n->args = {std::move(a), std::move(b)};
				return n;
			}

			template<typename TNext>
			std::shared_ptr<Node> parse_level(std::string_view ops, TNext next_level)
			{
				auto lhs = next_level();
				while (lhs && peek().type == Token::kOp && peek().text.size() == 1 && ops.find(peek().text[0]) != std::string_view::npos)
				{
					const char op = next().text[0];
					lhs = binary(op, lhs, next_level());
				}
				return lhs;
			}

			std::shared_ptr<Node> parse_or() { return parse_level("|", [this] { return parse_xor(); }); }
			std::shared_ptr<Node> parse_xor() { return parse_level("^", [this] { return parse_and(); }); }
			std::shared_ptr<Node> parse_and() { return parse_level("&", [this] { return parse_compare(); }); }
			std::shared_ptr<Node> parse_compare() { return parse_level("<>", [this] { return parse_add(); }); }
			std::shared_ptr<Node> parse_add() { return parse_level("+-", [this] { return parse_mul(); }); }
			std::shared_ptr<Node> parse_mul() { return parse_level("*/", [this] { return parse_unary(); }); }

			std::shared_ptr<Node> parse_unary()
			{
				if (is_op("!") || is_op("-"))
				{
					const char op = next().text[0];
					auto operand = parse_unary();
					if (!operand)
						return {};
					auto n = std::make_shared<Node>();
					n->kind = op == '!' ? Node::kNot : Node::kNeg;
					n->args = {std::move(operand)};
					return n;
				}
				return parse_primary();
			}

			std::shared_ptr<Node> parse_primary()
			{
				const Token& t = next();
				switch (t.type)
				{
				case Token::kInput:
				{
					auto n = std::make_shared<Node>();
					n->kind = Node::kLeaf;
					const auto colon = t.text.find(':');
					if (colon == std::string::npos)
					{
						fail("input `" + t.text + "` needs a device: `Device:Input`", t.pos);
						return {};
					}
					n->leaf.device_text = std::string(trim(std::string_view(t.text).substr(0, colon)));
					n->leaf.input_text = std::string(trim(std::string_view(t.text).substr(colon + 1)));
					if (n->leaf.device_text.empty() || n->leaf.input_text.empty())
					{
						fail("input `" + t.text + "` needs a device and an input: `Device:Input`", t.pos);
						return {};
					}
					resolve(n->leaf, m_devices);
					return n;
				}
				case Token::kNumber:
				{
					auto n = std::make_shared<Node>();
					n->kind = Node::kNumber;
					n->number = t.number;
					return n;
				}
				case Token::kIdent:
				{
					static const std::pair<std::string_view, size_t> kFunctions[] = {
						{"min", 2}, {"max", 2}, {"clamp", 3}, {"abs", 1}, {"deadzone", 2},
						{"toggle", 1}, {"hold", 2}, {"pulse", 2}, {"timer", 1},
					};
					const auto it = std::find_if(std::begin(kFunctions), std::end(kFunctions), [&](const auto& f) { return f.first == t.text; });
					if (it == std::end(kFunctions))
					{
						fail("unknown function '" + t.text + "' (inputs are written in backticks: `Device:Input`)", t.pos);
						return {};
					}
					if (!is_op("("))
					{
						fail("expected ( after " + t.text, peek().pos);
						return {};
					}
					next();
					auto n = std::make_shared<Node>();
					n->kind = Node::kFunc;
					n->func = t.text;
					if (!is_op(")"))
					{
						while (true)
						{
							auto arg = parse_or();
							if (!arg)
								return {};
							n->args.emplace_back(std::move(arg));
							if (is_op(","))
							{
								next();
								continue;
							}
							break;
						}
					}
					if (!is_op(")"))
					{
						fail("expected ) to close " + t.text + "(", peek().pos);
						return {};
					}
					next();
					if (n->args.size() != it->second)
					{
						fail(t.text + "() takes " + std::to_string(it->second) + " argument(s)", t.pos);
						return {};
					}
					return n;
				}
				case Token::kOp:
					if (t.text == "(")
					{
						auto inner = parse_or();
						if (!inner)
							return {};
						if (!is_op(")"))
						{
							fail("expected )", peek().pos);
							return {};
						}
						next();
						return inner;
					}
					fail("unexpected '" + t.text + "'", t.pos);
					return {};
				case Token::kEnd:
				default:
					fail("unexpected end", t.pos);
					return {};
				}
			}

			std::string_view m_text;
			const DeviceSet& m_devices;
			std::vector<Token> m_tokens;
			size_t m_index = 0;
			std::string m_error;
		};

		// ---- printing ----

		int precedence(const Node& n)
		{
			if (n.kind != Node::kBinary)
				return 10;
			switch (n.op)
			{
			case '|': return 1;
			case '^': return 2;
			case '&': return 3;
			case '<':
			case '>': return 4;
			case '+':
			case '-': return 5;
			case '*':
			case '/': return 6;
			default: return 10;
			}
		}

		template<typename TLeafText>
		std::string print(const Node& n, const TLeafText& leaf_text)
		{
			switch (n.kind)
			{
			case Node::kLeaf: return leaf_text(n.leaf);
			case Node::kNumber: return format_number(n.number);
			case Node::kNot:
			case Node::kNeg:
			{
				std::string inner = print(*n.args[0], leaf_text);
				if (n.args[0]->kind == Node::kBinary)
					inner = "(" + inner + ")";
				return (n.kind == Node::kNot ? "!" : "-") + inner;
			}
			case Node::kBinary:
			{
				const int p = precedence(n);
				std::string lhs = print(*n.args[0], leaf_text);
				std::string rhs = print(*n.args[1], leaf_text);
				if (precedence(*n.args[0]) < p)
					lhs = "(" + lhs + ")";
				const bool right_assoc_issue = precedence(*n.args[1]) == p && n.op != '|' && n.op != '&' && n.op != '+' && n.op != '*';
				if (precedence(*n.args[1]) < p || right_assoc_issue)
					rhs = "(" + rhs + ")";
				return lhs + " " + n.op + " " + rhs;
			}
			case Node::kFunc:
			{
				std::string out = n.func + "(";
				for (size_t i = 0; i < n.args.size(); i++)
				{
					if (i)
						out += ", ";
					out += print(*n.args[i], leaf_text);
				}
				return out + ")";
			}
			}
			return {};
		}

		std::string canonical_leaf(const Leaf& leaf, const DeviceSet& devices)
		{
			if (leaf.resolved())
				return quote_input(device_key(devices, leaf.device), input_key(devices, leaf.device, *leaf.input));
			if (leaf.device != kNoDevice)
				return quote_input(device_key(devices, leaf.device), leaf.input_text);
			return quote_input(leaf.device_text, leaf.input_text);
		}

		std::string canonical(const std::shared_ptr<Node>& root, const DeviceSet& devices)
		{
			if (!root)
				return {};
			return print(*root, [&](const Leaf& l) { return canonical_leaf(l, devices); });
		}

		void collect_leaves(const Node& n, std::vector<Leaf>& out)
		{
			if (n.kind == Node::kLeaf)
				out.emplace_back(n.leaf);
			for (const auto& a : n.args)
				collect_leaves(*a, out);
		}

		bool is_list_node(const Node& n)
		{
			if (n.kind == Node::kLeaf)
				return true;
			return n.kind == Node::kBinary && n.op == '|' && is_list_node(*n.args[0]) && is_list_node(*n.args[1]);
		}

		std::shared_ptr<Node> make_leaf(size_t device, InputId id, const DeviceSet& devices)
		{
			auto n = std::make_shared<Node>();
			n->kind = Node::kLeaf;
			n->leaf.device = device;
			n->leaf.input = id;
			n->leaf.device_text = device_key(devices, device);
			n->leaf.input_text = input_key(devices, device, id);
			return n;
		}

		std::shared_ptr<Node> or_chain(const std::vector<std::shared_ptr<Node>>& items)
		{
			std::shared_ptr<Node> root;
			for (const auto& item : items)
			{
				if (!root)
				{
					root = item;
					continue;
				}
				auto n = std::make_shared<Node>();
				n->kind = Node::kBinary;
				n->op = '|';
				n->args = {root, item};
				root = n;
			}
			return root;
		}

		void collect_list_items(const std::shared_ptr<Node>& n, std::vector<std::shared_ptr<Node>>& out)
		{
			if (n->kind == Node::kLeaf)
			{
				out.emplace_back(n);
				return;
			}
			collect_list_items(n->args[0], out);
			collect_list_items(n->args[1], out);
		}

		// ---- evaluation ----

		float eval_value(const Node& n, const InputSource& src);

		bool eval_active(const Node& n, const InputSource& src)
		{
			switch (n.kind)
			{
			case Node::kLeaf:
				return n.leaf.resolved() && src.active(n.leaf.device, *n.leaf.input);
			case Node::kNumber:
				return n.number > 0.5;
			case Node::kNot:
				return !eval_active(*n.args[0], src);
			case Node::kBinary:
				switch (n.op)
				{
				case '|': { const bool a = eval_active(*n.args[0], src), b = eval_active(*n.args[1], src); return a || b; } // both evaluated (stateful functions)
				case '&': { const bool a = eval_active(*n.args[0], src), b = eval_active(*n.args[1], src); return a && b; }
				case '^': return eval_active(*n.args[0], src) != eval_active(*n.args[1], src);
				default: break;
				}
				break;
			default:
				break;
			}
			return eval_value(n, src) > 0.5f;
		}

		float eval_func(const Node& n, const InputSource& src)
		{
			const auto now = Clock::now();
			const auto seconds_since = [&](Clock::time_point t) { return std::chrono::duration<double>(now - t).count(); };
			if (n.func == "min")
				return std::min(eval_value(*n.args[0], src), eval_value(*n.args[1], src));
			if (n.func == "max")
				return std::max(eval_value(*n.args[0], src), eval_value(*n.args[1], src));
			if (n.func == "clamp")
			{
				const float lo = eval_value(*n.args[1], src), hi = eval_value(*n.args[2], src);
				return std::clamp(eval_value(*n.args[0], src), std::min(lo, hi), std::max(lo, hi));
			}
			if (n.func == "abs")
				return std::abs(eval_value(*n.args[0], src));
			if (n.func == "deadzone")
			{
				const float x = eval_value(*n.args[0], src);
				const float dz = std::clamp(eval_value(*n.args[1], src), 0.0f, 0.999f);
				const float m = std::max(0.0f, std::abs(x) - dz) / (1.0f - dz);
				return x < 0 ? -m : m;
			}
			if (n.func == "timer")
			{
				const double period = eval_value(*n.args[0], src);
				if (period <= 0)
					return 0;
				const double t = std::chrono::duration<double>(now.time_since_epoch()).count();
				return (float)(std::fmod(t, period) / period);
			}

			// stateful
			const bool in = eval_active(*n.args[0], src);
			const double seconds = n.args.size() > 1 ? eval_value(*n.args[1], src) : 0.0;
			std::scoped_lock lock(n.state_mutex);
			const bool rising = in && !n.last_active;
			n.last_active = in;
			if (n.func == "toggle")
			{
				if (rising)
					n.state = !n.state;
				return n.state ? 1.0f : 0.0f;
			}
			if (n.func == "hold")
			{
				if (rising)
					n.since = now;
				return in && seconds_since(n.since) >= seconds ? 1.0f : 0.0f;
			}
			if (n.func == "pulse")
			{
				if (rising)
				{
					n.since = now;
					n.state = true;
				}
				if (n.state && seconds_since(n.since) >= seconds)
					n.state = false;
				return n.state ? 1.0f : 0.0f;
			}
			return 0;
		}

		float eval_value(const Node& n, const InputSource& src)
		{
			switch (n.kind)
			{
			case Node::kLeaf:
				return n.leaf.resolved() ? src.value(n.leaf.device, *n.leaf.input) : 0.0f;
			case Node::kNumber:
				return (float)n.number;
			case Node::kNot:
				return 1.0f - std::clamp(eval_value(*n.args[0], src), 0.0f, 1.0f);
			case Node::kNeg:
				return -eval_value(*n.args[0], src);
			case Node::kBinary:
			{
				if (n.op == '^')
					return eval_active(*n.args[0], src) != eval_active(*n.args[1], src) ? 1.0f : 0.0f;
				const float a = eval_value(*n.args[0], src);
				const float b = eval_value(*n.args[1], src);
				switch (n.op)
				{
				case '|': return std::abs(a) >= std::abs(b) ? a : b; // the stronger
				case '&': return std::abs(a) <= std::abs(b) ? a : b; // the weaker
				case '<': return a < b ? 1.0f : 0.0f;
				case '>': return a > b ? 1.0f : 0.0f;
				case '+': return a + b;
				case '-': return a - b;
				case '*': return a * b;
				case '/': return b == 0 ? 0.0f : a / b;
				}
				return 0;
			}
			case Node::kFunc:
				return eval_func(n, src);
			}
			return 0;
		}
	}

	std::string quote_input(std::string_view device_text, std::string_view input_text)
	{
		return "`" + escape(device_text) + ":" + escape(input_text) + "`";
	}

	std::string device_key(const DeviceSet& devices, size_t dev)
	{
		return devices.device_api(dev) + "/" + devices.device_uuid(dev);
	}

	std::string input_key(const DeviceSet& devices, size_t dev, InputId id)
	{
		const auto name = devices.input_name(dev, id);
		if (!name.empty() && name[0] != '#' && trim(name) == name && name.find(':') == std::string::npos)
		{
			if (const auto back = devices.find_input(dev, name); back && *back == id)
				return name;
		}
		return "#" + std::to_string(id);
	}

	Expression Expression::parse(std::string_view text, const DeviceSet& devices)
	{
		Expression e;
		std::string error;
		Parser parser(text, devices);
		e.m_root = parser.parse(error);
		if (!error.empty())
		{
			e.m_root.reset();
			e.m_error = error;
			e.m_text = std::string(trim(text));
			return e;
		}
		e.m_text = canonical(e.m_root, devices);
		return e;
	}

	Expression Expression::from_input(size_t device, InputId id, const DeviceSet& devices)
	{
		Expression e;
		e.m_root = make_leaf(device, id, devices);
		e.m_text = canonical(e.m_root, devices);
		return e;
	}

	std::vector<std::string> Expression::warnings() const
	{
		std::vector<std::string> result;
		for (const auto& l : leaves())
		{
			if (l.device == kNoDevice)
				result.emplace_back("device not attached: " + l.device_text);
			else if (!l.input)
				result.emplace_back(l.device_text + ": unknown input '" + l.input_text + "'");
		}
		return result;
	}

	float Expression::value(const InputSource& source) const
	{
		return m_root ? eval_value(*m_root, source) : 0.0f;
	}

	bool Expression::active(const InputSource& source) const
	{
		return m_root ? eval_active(*m_root, source) : false;
	}

	std::string Expression::display(const DeviceSet& devices, bool with_device) const
	{
		if (!m_root)
			return m_text;
		return print(*m_root, [&](const Leaf& l) {
			std::string name = l.resolved() ? devices.input_name(l.device, *l.input) : l.input_text;
			if (!l.resolved())
				name += " (?)";
			if (with_device || l.device == kNoDevice)
				name += " [" + (l.device == kNoDevice ? l.device_text : devices.device_display_name(l.device)) + "]";
			return name;
		});
	}

	bool Expression::is_list() const
	{
		if (!m_root)
			return m_error.empty();
		return is_list_node(*m_root);
	}

	std::vector<Leaf> Expression::leaves() const
	{
		std::vector<Leaf> result;
		if (m_root)
			collect_leaves(*m_root, result);
		return result;
	}

	bool Expression::references_device(size_t dev) const
	{
		const auto l = leaves();
		return std::any_of(l.cbegin(), l.cend(), [dev](const Leaf& leaf) { return leaf.device == dev; });
	}

	Expression Expression::append(const Expression& expr, size_t device, InputId id, const DeviceSet& devices)
	{
		if (!expr.m_root)
			return from_input(device, id, devices);
		for (const auto& l : expr.leaves())
		{
			if (l.device == device && l.input == id)
				return expr;
		}
		Expression e;
		e.m_root = or_chain({expr.m_root, make_leaf(device, id, devices)});
		e.m_text = canonical(e.m_root, devices);
		return e;
	}

	Expression Expression::remove_device(const Expression& expr, size_t device, const DeviceSet& devices)
	{
		if (!expr.m_root || !expr.is_list())
			return expr;
		std::vector<std::shared_ptr<Node>> items, kept;
		collect_list_items(expr.m_root, items);
		for (const auto& item : items)
		{
			if (item->leaf.device != device)
				kept.emplace_back(item);
		}
		Expression e;
		e.m_root = or_chain(kept);
		e.m_text = canonical(e.m_root, devices);
		return e;
	}
}
