/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2012  Claire Xenia Wolf <claire@yosyshq.com>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "kernel/register.h"
#include "kernel/celltypes.h"
#include "kernel/log.h"
#include <string.h>

#ifndef _WIN32
#  include <dirent.h>
#endif

#ifdef __APPLE__
#  include <unistd.h>
#endif

#ifdef YOSYS_ENABLE_READLINE
#  include <readline/readline.h>
#endif

#ifdef YOSYS_ENABLE_EDITLINE
#  include <editline/readline.h>
#endif

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

#undef CLUSTER_CELLS_AND_PORTBOXES

struct ShowWorker
{
	CellTypes ct;

	vector<shared_str> dot_escape_store;
	std::map<RTLIL::IdString, int> dot_id2num_store;
	std::map<RTLIL::IdString, int> autonames;
	int single_idx_count;

	struct net_conn { std::set<std::pair<std::string, int>> in, out; std::string color; };
	std::map<std::string, net_conn> net_conn_map;

	FILE *f;
	RTLIL::Design *design;
	RTLIL::Module *module;
	uint32_t currentColor;
	bool genWidthLabels;
	bool genSignedLabels;
	bool stretchIO;
	bool enumerateIds;
	bool abbreviateIds;
	bool notitle;
	int page_counter;

	const std::vector<std::pair<std::string, RTLIL::Selection>> &color_selections;
	const std::vector<std::pair<std::string, RTLIL::Selection>> &label_selections;

	std::map<RTLIL::Const, int> colorattr_cache;
	RTLIL::IdString colorattr;


	static uint32_t xorshift32(uint32_t x) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		return x;
	}

	std::string nextColor()
	{
		if (currentColor == 0)
			return "color=\"black\"";
		return stringf("colorscheme=\"dark28\", color=\"%d\", fontcolor=\"%d\"", currentColor%8+1, currentColor%8+1);
	}

	std::string nextColor(std::string presetColor)
	{
		if (presetColor.empty())
			return nextColor();
		return presetColor;
	}

	std::string nextColor(RTLIL::SigSpec sig, std::string defaultColor)
	{
		sig.sort_and_unify();
		for (auto &c : sig.chunks()) {
			if (c.wire != nullptr)
				for (auto &s : color_selections)
					if (s.second.selected_members.count(module->name) > 0 && s.second.selected_members.at(module->name).count(c.wire->name) > 0)
						return stringf("color=\"%s\"", s.first.c_str());
		}
		return defaultColor;
	}

	std::string nextColor(const RTLIL::SigSig &conn, std::string defaultColor)
	{
		return nextColor(conn.first, nextColor(conn.second, defaultColor));
	}

	std::string nextColor(const RTLIL::SigSpec &sig)
	{
		return nextColor(sig, nextColor());
	}

	std::string nextColor(const RTLIL::SigSig &conn)
	{
		return nextColor(conn, nextColor());
	}

	std::string widthLabel(int bits)
	{
		if (bits <= 1)
			return "label=\"\"";
		if (!genWidthLabels)
			return "style=\"setlinewidth(3)\", label=\"\"";
		return stringf("style=\"setlinewidth(3)\", label=\"<%d>\"", bits);
	}

	const char *findColor(std::string member_name)
	{
		for (auto &s : color_selections)
			if (s.second.selected_member(module->name, member_name)) {
				dot_escape_store.push_back(stringf(", color=\"%s\"", s.first.c_str()));
				return dot_escape_store.back().c_str();
			}

		RTLIL::Const colorattr_value;
		RTLIL::Cell *cell = module->cell(member_name);
		RTLIL::Wire *wire = module->wire(member_name);

		if (cell && cell->attributes.count(colorattr))
			colorattr_value = cell->attributes.at(colorattr);
		else if (wire && wire->attributes.count(colorattr))
			colorattr_value = wire->attributes.at(colorattr);
		else
			return "";

		if (colorattr_cache.count(colorattr_value) == 0) {
			int next_id = GetSize(colorattr_cache);
			colorattr_cache[colorattr_value] = (next_id % 8) + 1;
		}

		dot_escape_store.push_back(stringf(", colorscheme=\"dark28\", color=\"%d\", fontcolor=\"%d\"", colorattr_cache.at(colorattr_value), colorattr_cache.at(colorattr_value)));
		return dot_escape_store.back().c_str();
	}

	const char *findLabel(std::string member_name)
	{
		for (auto &s : label_selections)
			if (s.second.selected_member(module->name, member_name))
				return escape(s.first);
		return escape(member_name, true);
	}

	const char *escape(std::string id, bool is_name = false)
	{
		if (id.size() == 0)
			return "";

		if (id[0] == '$' && is_name) {
			if (enumerateIds) {
				if (autonames.count(id) == 0) {
					autonames[id] = autonames.size() + 1;
					log("Generated short name for internal identifier: _%d_ -> %s\n", autonames[id], id.c_str());
				}
				id = stringf("_%d_", autonames[id]);
			} else if (abbreviateIds) {
				const char *p = id.c_str();
				const char *q = strrchr(p, '$');
				id = std::string(q);
			}
		}

		if (id[0] == '\\')
			id = id.substr(1);

		std::string str;
		for (char ch : id) {
			if (ch == '\\') {
				 // new graphviz have bug with escaping '\'
				str += "&#9586;";
				continue;
			}
			if (ch == '"')
				str += "\\";
			str += ch;
		}

		dot_escape_store.push_back(str);
		return dot_escape_store.back().c_str();
	}

	int id2num(RTLIL::IdString id)
	{
		if (dot_id2num_store.count(id) > 0)
			return dot_id2num_store[id];
		return dot_id2num_store[id] = dot_id2num_store.size() + 1;
	}

	std::string gen_signode_simple(RTLIL::SigSpec sig, bool range_check = true)
	{
		if (GetSize(sig) == 0) {
			fprintf(f, "v%d [ label=\"\" ];\n", single_idx_count);
			return stringf("v%d", single_idx_count++);
		}

		if (sig.is_chunk()) {
			const RTLIL::SigChunk &c = sig.as_chunk();
			if (c.wire != nullptr && design->selected_member(module->name, c.wire->name)) {
				if (!range_check || c.wire->width == c.width)
						return stringf("n%d", id2num(c.wire->name));
			} else {
				fprintf(f, "v%d [ label=\"%s\" ];\n", single_idx_count, findLabel(log_signal(c)));
				return stringf("v%d", single_idx_count++);
			}
		}

		return std::string();
	}

	std::string gen_portbox(std::string port, RTLIL::SigSpec sig, bool driver, std::string *node = nullptr)
	{
		std::string code;
		std::string net = gen_signode_simple(sig);
		if (net.empty())
		{
			std::string label_string;
			int pos = sig.size()-1;
			int idx = single_idx_count++;
			for (int rep, i = int(sig.chunks().size())-1; i >= 0; i -= rep) {
				const RTLIL::SigChunk &c = sig.chunks().at(i);
				int cl, cr;
				if (c.wire) {
					if (c.wire->upto) {
						cr = c.wire->start_offset + (c.wire->width - c.offset - 1);
						cl = cr - (c.width - 1);
					} else {
						cr = c.wire->start_offset + c.offset;
						cl = cr + c.width - 1;
					}
				} else {
					cl = c.offset + c.width - 1;
					cr = c.offset;
				}
				if (!driver && c.wire == nullptr) {
					RTLIL::State s1 = c.data.front();
					for (auto s2 : c.data)
						if (s1 != s2)
							goto not_const_stream;
					net.clear();
				} else {
			not_const_stream:
					net = gen_signode_simple(c, false);
					log_assert(!net.empty());
				}
				for (rep = 1; i-rep >= 0 && c == sig.chunks().at(i-rep); rep++) {}
				std::string repinfo = rep > 1 ? stringf("%dx ", rep) : "";
				if (driver) {
					log_assert(!net.empty());
					label_string += stringf("<s%d> %d:%d - %s%d:%d |", i, pos, pos-c.width+1, repinfo.c_str(), cl, cr);
					net_conn_map[net].in.insert({stringf("x%d:s%d", idx, i), rep*c.width});
					net_conn_map[net].color = nextColor(c, net_conn_map[net].color);
				} else
				if (net.empty()) {
					log_assert(rep == 1);
					label_string += stringf("%c -&gt; %d:%d |",
							c.data.front() == State::S0 ? '0' :
							c.data.front() == State::S1 ? '1' :
							c.data.front() == State::Sx ? 'X' :
							c.data.front() == State::Sz ? 'Z' : '?',
							pos, pos-rep*c.width+1);
				} else {
					label_string += stringf("<s%d> %s%d:%d - %d:%d |", i, repinfo.c_str(), cl, cr, pos, pos-rep*c.width+1);
					net_conn_map[net].out.insert({stringf("x%d:s%d", idx, i), rep*c.width});
					net_conn_map[net].color = nextColor(c, net_conn_map[net].color);
				}
				pos -= rep * c.width;
			}
			if (label_string[label_string.size()-1] == '|')
				label_string = label_string.substr(0, label_string.size()-1);
			code += stringf("x%d [ shape=record, style=rounded, label=\"%s\" ];\n", idx, label_string.c_str());
			if (!port.empty()) {
				currentColor = xorshift32(currentColor);
				log_warning("WIDTHLABEL %s %d\n", log_signal(sig), GetSize(sig));
				if (driver)
					code += stringf("%s:e -> x%d:w [arrowhead=odiamond, arrowtail=odiamond, dir=both, %s, %s];\n", port.c_str(), idx, nextColor(sig).c_str(), widthLabel(sig.size()).c_str());
				else
					code += stringf("x%d:e -> %s:w [arrowhead=odiamond, arrowtail=odiamond, dir=both, %s, %s];\n", idx, port.c_str(), nextColor(sig).c_str(), widthLabel(sig.size()).c_str());
			}
			if (node != nullptr)
				*node = stringf("x%d", idx);
		}
		else
		{
			if (!port.empty()) {
				if (driver)
					net_conn_map[net].in.insert({port, GetSize(sig)});
				else
					net_conn_map[net].out.insert({port, GetSize(sig)});
				net_conn_map[net].color = nextColor(sig, net_conn_map[net].color);
			}
			if (node != nullptr)
				*node = net;
		}
		return code;
	}

	void collect_proc_signals(std::vector<RTLIL::SigSpec> &obj, std::set<RTLIL::SigSpec> &signals)
	{
		for (auto &it : obj)
			if (!it.is_fully_const())
				signals.insert(it);
	}

	void collect_proc_signals(std::vector<RTLIL::SigSig> &obj, std::set<RTLIL::SigSpec> &input_signals, std::set<RTLIL::SigSpec> &output_signals)
	{
		for (auto &it : obj) {
			output_signals.insert(it.first);
			if (!it.second.is_fully_const())
				input_signals.insert(it.second);
		}
	}

	void collect_proc_signals(RTLIL::CaseRule *obj, std::set<RTLIL::SigSpec> &input_signals, std::set<RTLIL::SigSpec> &output_signals)
	{
		collect_proc_signals(obj->compare, input_signals);
		collect_proc_signals(obj->actions, input_signals, output_signals);
		for (auto it : obj->switches)
			collect_proc_signals(it, input_signals, output_signals);
	}

	void collect_proc_signals(RTLIL::SwitchRule *obj, std::set<RTLIL::SigSpec> &input_signals, std::set<RTLIL::SigSpec> &output_signals)
	{
		input_signals.insert(obj->signal);
		for (auto it : obj->cases)
			collect_proc_signals(it, input_signals, output_signals);
	}

	void collect_proc_signals(RTLIL::SyncRule *obj, std::set<RTLIL::SigSpec> &input_signals, std::set<RTLIL::SigSpec> &output_signals)
	{
		input_signals.insert(obj->signal);
		collect_proc_signals(obj->actions, input_signals, output_signals);
		for (auto it : obj->mem_write_actions) {
			input_signals.insert(it.address);
			input_signals.insert(it.data);
			input_signals.insert(it.enable);
		}
	}

	void collect_proc_signals(RTLIL::Process *obj, std::set<RTLIL::SigSpec> &input_signals, std::set<RTLIL::SigSpec> &output_signals)
	{
		collect_proc_signals(&obj->root_case, input_signals, output_signals);
		for (auto it : obj->syncs)
			collect_proc_signals(it, input_signals, output_signals);
	}

	void handle_module()
	{
		single_idx_count = 0;
		dot_escape_store.clear();
		dot_id2num_store.clear();
		net_conn_map.clear();

		fprintf(f, "digraph \"%s\" {\n", escape(module->name.str()));
		if (!notitle)
			fprintf(f, "label=\"%s\";\n", escape(module->name.str()));
		fprintf(f, "rankdir=\"LR\";\n");
		fprintf(f, "remincross=true;\n");

		std::set<std::string> all_sources, all_sinks;

		std::map<std::string, std::string> wires_on_demand;
		for (auto wire : module->selected_wires()) {
			const char *shape = "diamond";
			if (wire->port_input || wire->port_output)
				shape = "octagon";
			if (wire->name.isPublic()) {
				fprintf(f, "n%d [ shape=%s, label=\"%s\", %s, fontcolor=\"black\" ];\n",
						id2num(wire->name), shape, findLabel(wire->name.str()),
						nextColor(RTLIL::SigSpec(wire), "color=\"black\"").c_str());
				if (wire->port_input)
					all_sources.insert(stringf("n%d", id2num(wire->name)));
				else if (wire->port_output)
					all_sinks.insert(stringf("n%d", id2num(wire->name)));
			} else {
				wires_on_demand[stringf("n%d", id2num(wire->name))] = wire->name.str();
			}
		}

		if (stretchIO)
		{
			fprintf(f, "{ rank=\"source\";");
			for (auto n : all_sources)
				fprintf(f, " %s;", n.c_str());
			fprintf(f, "}\n");

			fprintf(f, "{ rank=\"sink\";");
			for (auto n : all_sinks)
				fprintf(f, " %s;", n.c_str());
			fprintf(f, "}\n");
		}

		for (auto cell : module->selected_cells())
		{
			std::vector<RTLIL::IdString> in_ports, out_ports;

			for (auto &conn : cell->connections()) {
				if (!ct.cell_output(cell->type, conn.first))
					in_ports.push_back(conn.first);
				else
					out_ports.push_back(conn.first);
			}

			std::sort(in_ports.begin(), in_ports.end(), RTLIL::sort_by_id_str());
			std::sort(out_ports.begin(), out_ports.end(), RTLIL::sort_by_id_str());

			std::string label_string = "{{";

			for (auto &p : in_ports)
				label_string += stringf("<p%d> %s%s|", id2num(p), escape(p.str()),
						genSignedLabels && cell->hasParam(p.str() + "_SIGNED") &&
						cell->getParam(p.str() + "_SIGNED").as_bool() ? "*" : "");
			if (label_string[label_string.size()-1] == '|')
				label_string = label_string.substr(0, label_string.size()-1);

			label_string += stringf("}|%s\\n%s|{", findLabel(cell->name.str()), escape(cell->type.str()));

			for (auto &p : out_ports)
				label_string += stringf("<p%d> %s|", id2num(p), escape(p.str()));
			if (label_string[label_string.size()-1] == '|')
				label_string = label_string.substr(0, label_string.size()-1);

			label_string += "}}";

			std::string code;
			for (auto &conn : cell->connections()) {
				code += gen_portbox(stringf("c%d:p%d", id2num(cell->name), id2num(conn.first)),
						conn.second, ct.cell_output(cell->type, conn.first));
			}

#ifdef CLUSTER_CELLS_AND_PORTBOXES
			if (!code.empty())
				fprintf(f, "subgraph cluster_c%d {\nc%d [ shape=record, label=\"%s\"%s ];\n%s}\n",
						id2num(cell->name), id2num(cell->name), label_string.c_str(), findColor(cell->name), code.c_str());
			else
#endif
				fprintf(f, "c%d [ shape=record, label=\"%s\"%s ];\n%s",
						id2num(cell->name), label_string.c_str(), findColor(cell->name.str()), code.c_str());
		}

		for (auto &it : module->processes)
		{
			RTLIL::Process *proc = it.second;

			if (!design->selected_member(module->name, proc->name))
				continue;

			std::set<RTLIL::SigSpec> input_signals, output_signals;
			collect_proc_signals(proc, input_signals, output_signals);

			int pidx = single_idx_count++;
			input_signals.erase(RTLIL::SigSpec());
			output_signals.erase(RTLIL::SigSpec());

			for (auto &sig : input_signals) {
				std::string code, node;
				code += gen_portbox("", sig, false, &node);
				fprintf(f, "%s", code.c_str());
				net_conn_map[node].out.insert({stringf("p%d", pidx), GetSize(sig)});
				net_conn_map[node].color = nextColor(sig, net_conn_map[node].color);
			}

			for (auto &sig : output_signals) {
				std::string code, node;
				code += gen_portbox("", sig, true, &node);
				fprintf(f, "%s", code.c_str());
				net_conn_map[node].in.insert({stringf("p%d", pidx), GetSize(sig)});
				net_conn_map[node].color = nextColor(sig, net_conn_map[node].color);
			}

			std::string proc_src = RTLIL::unescape_id(proc->name);
			if (proc->attributes.count(ID::src) > 0)
				proc_src = proc->attributes.at(ID::src).decode_string();
			fprintf(f, "p%d [shape=box, style=rounded, label=\"PROC %s\\n%s\"];\n", pidx, findLabel(proc->name.str()), proc_src.c_str());
		}

		for (auto &conn : module->connections())
		{
			bool found_lhs_wire = false;
			for (auto &c : conn.first.chunks()) {
				if (c.wire == nullptr || design->selected_member(module->name, c.wire->name))
					found_lhs_wire = true;
			}
			bool found_rhs_wire = false;
			for (auto &c : conn.second.chunks()) {
				if (c.wire == nullptr || design->selected_member(module->name, c.wire->name))
					found_rhs_wire = true;
			}
			if (!found_lhs_wire || !found_rhs_wire)
				continue;

			std::string code, left_node, right_node;
			code += gen_portbox("", conn.second, false, &left_node);
			code += gen_portbox("", conn.first, true, &right_node);
			fprintf(f, "%s", code.c_str());

			if (left_node[0] == 'x' && right_node[0] == 'x') {
				currentColor = xorshift32(currentColor);
				fprintf(f, "%s:e -> %s:w [arrowhead=odiamond, arrowtail=odiamond, dir=both, %s, %s];\n", left_node.c_str(), right_node.c_str(), nextColor(conn).c_str(), widthLabel(conn.first.size()).c_str());
			} else {
				net_conn_map[right_node].color = nextColor(conn, net_conn_map[right_node].color);
				net_conn_map[left_node].color = nextColor(conn, net_conn_map[left_node].color);
				if (left_node[0] == 'x') {
					net_conn_map[right_node].in.insert({left_node, GetSize(conn.first)});
				} else if (right_node[0] == 'x') {
					net_conn_map[left_node].out.insert({right_node, GetSize(conn.first)});
				} else {
					net_conn_map[right_node].in.insert({stringf("x%d:e", single_idx_count), GetSize(conn.first)});
					net_conn_map[left_node].out.insert({stringf("x%d:w", single_idx_count), GetSize(conn.first)});
					fprintf(f, "x%d [shape=box, style=rounded, label=\"BUF\"];\n", single_idx_count++);
				}
			}
		}

		for (auto &it : net_conn_map)
		{
			currentColor = xorshift32(currentColor);
			if (wires_on_demand.count(it.first) > 0) {
				if (it.second.in.size() == 1 && it.second.out.size() > 1 && it.second.in.begin()->first.compare(0, 1, "p") == 0)
					it.second.out.erase(*it.second.in.begin());
				if (it.second.in.size() == 1 && it.second.out.size() == 1) {
					std::string from = it.second.in.begin()->first, to = it.second.out.begin()->first;
					int bits = it.second.in.begin()->second;
					if (from != to || from.compare(0, 1, "p") != 0)
						fprintf(f, "%s:e -> %s:w [%s, %s];\n", from.c_str(), to.c_str(), nextColor(it.second.color).c_str(), widthLabel(bits).c_str());
					continue;
				}
				if (it.second.in.size() == 0 || it.second.out.size() == 0)
					fprintf(f, "%s [ shape=diamond, label=\"%s\" ];\n", it.first.c_str(), findLabel(wires_on_demand[it.first]));
				else
					fprintf(f, "%s [ shape=point ];\n", it.first.c_str());
			}
			for (auto &it2 : it.second.in)
				fprintf(f, "%s:e -> %s:w [%s, %s];\n", it2.first.c_str(), it.first.c_str(), nextColor(it.second.color).c_str(), widthLabel(it2.second).c_str());
			for (auto &it2 : it.second.out)
				fprintf(f, "%s:e -> %s:w [%s, %s];\n", it.first.c_str(), it2.first.c_str(), nextColor(it.second.color).c_str(), widthLabel(it2.second).c_str());
		}

		fprintf(f, "}\n");
	}

	ShowWorker(FILE *f, RTLIL::Design *design, std::vector<RTLIL::Design*> &libs, uint32_t colorSeed, bool genWidthLabels,
			bool genSignedLabels, bool stretchIO, bool enumerateIds, bool abbreviateIds, bool notitle,
			const std::vector<std::pair<std::string, RTLIL::Selection>> &color_selections,
			const std::vector<std::pair<std::string, RTLIL::Selection>> &label_selections, RTLIL::IdString colorattr) :
			f(f), design(design), currentColor(colorSeed), genWidthLabels(genWidthLabels),
			genSignedLabels(genSignedLabels), stretchIO(stretchIO), enumerateIds(enumerateIds), abbreviateIds(abbreviateIds),
			notitle(notitle), color_selections(color_selections), label_selections(label_selections), colorattr(colorattr)
	{
		ct.setup_internals();
		ct.setup_internals_mem();
		ct.setup_internals_anyinit();
		ct.setup_stdcells();
		ct.setup_stdcells_mem();
		ct.setup_design(design);

		for (auto lib : libs)
			ct.setup_design(lib);

		design->optimize();
		page_counter = 0;
		for (auto mod : design->selected_modules())
		{
			module = mod;
			if (design->selected_whole_module(module->name)) {
				if (module->get_blackbox_attribute()) {
					// log("Skipping blackbox module %s.\n", log_id(module->name));
					continue;
				} else
				if (module->cells().size() == 0 && module->connections().empty() && module->processes.empty()) {
					log("Skipping empty module %s.\n", log_id(module->name));
					continue;
				} else
					log("Dumping module %s to page %d.\n", log_id(module->name), ++page_counter);
			} else
				log("Dumping selected parts of module %s to page %d.\n", log_id(module->name), ++page_counter);
			handle_module();
		}
	}
};

struct ShowPass : public Pass {
	ShowPass() : Pass("show", "generate schematics using graphviz") { }
	void help() override
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    show [options] [selection]\n");
		log("\n");
		log("Create a graphviz DOT file for the selected part of the design and compile it\n");
		log("to a graphics file (usually SVG or PostScript).\n");
		log("\n");
		log("    -viewer <viewer>\n");
		log("        Run the specified command with the graphics file as parameter.\n");
		log("        On Windows, this pauses yosys until the viewer exits.\n");
		log("\n");
		log("    -format <format>\n");
		log("        Generate a graphics file in the specified format. Use 'dot' to just\n");
		log("        generate a .dot file, or other <format> strings such as 'svg' or 'ps'\n");
		log("        to generate files in other formats (this calls the 'dot' command).\n");
		log("\n");
		log("    -lib <verilog_or_rtlil_file>\n");
		log("        Use the specified library file for determining whether cell ports are\n");
		log("        inputs or outputs. This option can be used multiple times to specify\n");
		log("        more than one library.\n");
		log("\n");
		log("        note: in most cases it is better to load the library before calling\n");
		log("        show with 'read_verilog -lib <filename>'. it is also possible to\n");
		log("        load liberty files with 'read_liberty -lib <filename>'.\n");
		log("\n");
		log("    -prefix <prefix>\n");
		log("        generate <prefix>.* instead of ~/.yosys_show.*\n");
		log("\n");
		log("    -color <color> <object>\n");
		log("        assign the specified color to the specified object. The object can be\n");
		log("        a single selection wildcard expressions or a saved set of objects in\n");
		log("        the @<name> syntax (see \"help select\" for details).\n");
		log("\n");
		log("    -label <text> <object>\n");
		log("        assign the specified label text to the specified object. The object can\n");
		log("        be a single selection wildcard expressions or a saved set of objects in\n");
		log("        the @<name> syntax (see \"help select\" for details).\n");
		log("\n");
		log("    -colors <seed>\n");
		log("        Randomly assign colors to the wires. The integer argument is the seed\n");
		log("        for the random number generator. Change the seed value if the colored\n");
		log("        graph still is ambiguous. A seed of zero deactivates the coloring.\n");
		log("\n");
		log("    -colorattr <attribute_name>\n");
		log("        Use the specified attribute to assign colors. A unique color is\n");
		log("        assigned to each unique value of this attribute.\n");
		log("\n");
		log("    -width\n");
		log("        annotate buses with a label indicating the width of the bus.\n");
		log("\n");
		log("    -signed\n");
		log("        mark ports (A, B) that are declared as signed (using the [AB]_SIGNED\n");
		log("        cell parameter) with an asterisk next to the port name.\n");
		log("\n");
		log("    -stretch\n");
		log("        stretch the graph so all inputs are on the left side and all outputs\n");
		log("        (including inout ports) are on the right side.\n");
		log("\n");
		log("    -pause\n");
		log("        wait for the user to press enter to before returning\n");
		log("\n");
		log("    -enum\n");
		log("        enumerate objects with internal ($-prefixed) names\n");
		log("\n");
		log("    -long\n");
		log("        do not abbreviate objects with internal ($-prefixed) names\n");
		log("\n");
		log("    -notitle\n");
		log("        do not add the module name as graph title to the dot file\n");
		log("\n");
		log("    -nobg\n");
		log("        don't run viewer in the background, IE wait for the viewer tool to\n");
		log("        exit before returning\n");
		log("\n");
		log("When no <format> is specified, 'dot' is used. When no <format> and <viewer> is\n");
		log("specified, 'xdot' is used to display the schematic (POSIX systems only).\n");
		log("\n");
		log("The generated output files are '~/.yosys_show.dot' and '~/.yosys_show.<format>',\n");
		log("unless another prefix is specified using -prefix <prefix>.\n");
		log("\n");
		log("Yosys on Windows and YosysJS use different defaults: The output is written\n");
		log("to 'show.dot' in the current directory and new viewer is launched each time\n");
		log("the 'show' command is executed.\n");
		log("\n");
	}
	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		log_header(design, "Generating Graphviz representation of design.\n");
		log_push();

		std::vector<std::pair<std::string, RTLIL::Selection>> color_selections;
		std::vector<std::pair<std::string, RTLIL::Selection>> label_selections;

#if defined(_WIN32) || defined(YOSYS_DISABLE_SPAWN)
		std::string format = "dot";
		std::string prefix = "show";
#else
		std::string format;
		std::string prefix = stringf("%s/.yosys_show", getenv("HOME") ? getenv("HOME") : ".");
#endif
		std::string viewer_exe;
		std::vector<std::string> libfiles;
		std::vector<RTLIL::Design*> libs;
		uint32_t colorSeed = 0;
		bool flag_width = false;
		bool flag_signed = false;
		bool flag_stretch = false;
		bool flag_pause = false;
		bool flag_enum = false;
		bool flag_abbreviate = true;
		bool flag_notitle = false;
		bool custom_prefix = false;
		std::string background = "&";
		RTLIL::IdString colorattr;

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++)
		{
			std::string arg = args[argidx];
			if (arg == "-viewer" && argidx+1 < args.size()) {
				viewer_exe = args[++argidx];
				continue;
			}
			if (arg == "-lib" && argidx+1 < args.size()) {
				libfiles.push_back(args[++argidx]);
				continue;
			}
			if (arg == "-prefix" && argidx+1 < args.size()) {
				prefix = args[++argidx];
				custom_prefix = true;
				continue;
			}
			if (arg == "-color" && argidx+2 < args.size()) {
				std::pair<std::string, RTLIL::Selection> data;
				data.first = args[++argidx], argidx++;
				handle_extra_select_args(this, args, argidx, argidx+1, design);
				data.second = design->selection_stack.back();
				design->selection_stack.pop_back();
				color_selections.push_back(data);
				continue;
			}
			if (arg == "-label" && argidx+2 < args.size()) {
				std::pair<std::string, RTLIL::Selection> data;
				data.first = args[++argidx], argidx++;
				handle_extra_select_args(this, args, argidx, argidx+1, design);
				data.second = design->selection_stack.back();
				design->selection_stack.pop_back();
				label_selections.push_back(data);
				continue;
			}
			if (arg == "-colors" && argidx+1 < args.size()) {
				colorSeed = atoi(args[++argidx].c_str());
				for (int i = 0; i < 100; i++)
					colorSeed = ShowWorker::xorshift32(colorSeed);
				continue;
			}
			if (arg == "-colorattr" && argidx+1 < args.size()) {
				colorattr = RTLIL::escape_id(args[++argidx]);
				continue;
			}
			if (arg == "-format" && argidx+1 < args.size()) {
				format = args[++argidx];
				continue;
			}
			if (arg == "-width") {
				flag_width= true;
				continue;
			}
			if (arg == "-signed") {
				flag_signed= true;
				continue;
			}
			if (arg == "-stretch") {
				flag_stretch= true;
				continue;
			}
			if (arg == "-pause") {
				flag_pause= true;
				continue;
			}
			if (arg == "-enum") {
				flag_enum = true;
				flag_abbreviate = false;
				continue;
			}
			if (arg == "-long") {
				flag_enum = false;
				flag_abbreviate = false;
				continue;
			}
			if (arg == "-notitle") {
				flag_notitle = true;
				continue;
			}
			if (arg == "-nobg") {
				background= "";
				continue;
			}
			break;
		}
		extra_args(args, argidx, design);

		if (format != "ps" && format != "dot") {
			int modcount = 0;
			for (auto module : design->selected_modules()) {
				if (module->get_blackbox_attribute())
					continue;
				if (module->cells().size() == 0 && module->connections().empty())
					continue;
				modcount++;
			}
			if (modcount > 1)
				log_cmd_error("For formats different than 'ps' or 'dot' only one module must be selected.\n");
		}

		for (auto filename : libfiles) {
			std::ifstream f;
			f.open(filename.c_str());
			yosys_input_files.insert(filename);
			if (f.fail())
				log_error("Can't open lib file `%s'.\n", filename.c_str());
			RTLIL::Design *lib = new RTLIL::Design;
			Frontend::frontend_call(lib, &f, filename, (filename.size() > 3 && filename.compare(filename.size()-3, std::string::npos, ".il") == 0 ? "rtlil" : "verilog"));
			libs.push_back(lib);
		}

		if (libs.size() > 0)
			log_header(design, "Continuing show pass.\n");

		std::string dot_file = stringf("%s.dot", prefix.c_str());
		std::string out_file = stringf("%s.%s", prefix.c_str(), format.empty() ? "svg" : format.c_str());

		log("Writing dot description to `%s'.\n", dot_file.c_str());
		FILE *f = fopen(dot_file.c_str(), "w");
		if (custom_prefix)
			yosys_output_files.insert(dot_file);
		if (f == nullptr) {
			for (auto lib : libs)
				delete lib;
			log_cmd_error("Can't open dot file `%s' for writing.\n", dot_file.c_str());
		}
		ShowWorker worker(f, design, libs, colorSeed, flag_width, flag_signed, flag_stretch, flag_enum, flag_abbreviate, flag_notitle, color_selections, label_selections, colorattr);
		fclose(f);

		for (auto lib : libs)
			delete lib;

		if (worker.page_counter == 0)
			log_cmd_error("Nothing there to show.\n");

		if (format != "dot" && !format.empty()) {
			#ifdef _WIN32
				// system()/cmd.exe does not understand single quotes on Windows.
				#define DOT_CMD "dot -T%s \"%s\" > \"%s.new\" && move \"%s.new\" \"%s\""
			#else
				#define DOT_CMD "dot -T%s '%s' > '%s.new' && mv '%s.new' '%s'"
			#endif
			std::string cmd = stringf(DOT_CMD, format.c_str(), dot_file.c_str(), out_file.c_str(), out_file.c_str(), out_file.c_str());
			#undef DOT_CMD
			log("Exec: %s\n", cmd.c_str());
			#if !defined(YOSYS_DISABLE_SPAWN)
				if (run_command(cmd) != 0)
					log_cmd_error("Shell command failed!\n");
			#endif
		}

		#if defined(YOSYS_DISABLE_SPAWN)
			log_assert(viewer_exe.empty() && !format.empty());
		#else
		if (!viewer_exe.empty()) {
			#ifdef _WIN32
				// system()/cmd.exe does not understand single quotes nor
				// background tasks on Windows. So we have to pause yosys
				// until the viewer exits.
				std::string cmd = stringf("%s \"%s\"", viewer_exe.c_str(), out_file.c_str());
			#else
				std::string cmd = stringf("%s '%s' %s", viewer_exe.c_str(), out_file.c_str(), background.c_str());
			#endif
			log("Exec: %s\n", cmd.c_str());
			if (run_command(cmd) != 0)
				log_cmd_error("Shell command failed!\n");
		} else
		if (format.empty()) {
			#ifdef __APPLE__
			std::string cmd = stringf("ps -fu %d | grep -q '[ ]%s' || xdot '%s' %s", getuid(), dot_file.c_str(), dot_file.c_str(), background.c_str());
			#else
			std::string cmd = stringf("{ test -f '%s.pid' && fuser -s '%s.pid' 2> /dev/null; } || ( echo $$ >&3; exec xdot '%s'; ) 3> '%s.pid' %s", dot_file.c_str(), dot_file.c_str(), dot_file.c_str(), dot_file.c_str(), background.c_str());
			#endif
			log("Exec: %s\n", cmd.c_str());
			if (run_command(cmd) != 0)
				log_cmd_error("Shell command failed!\n");
		}
		#endif

		if (flag_pause) {
		#ifdef YOSYS_ENABLE_READLINE
			char *input = nullptr;
			while ((input = readline("Press ENTER to continue (or type 'shell' to open a shell)> ")) != nullptr) {
				if (input[strspn(input, " \t\r\n")] == 0)
					break;
				char *p = input + strspn(input, " \t\r\n");
				if (!strcmp(p, "shell")) {
					Pass::call(design, "shell");
					break;
				}
			}
		#else
			log_cmd_error("This version of yosys is built without readline support => 'show -pause' is not available.\n");
		#endif
		}

		log_pop();
	}
} ShowPass;

PRIVATE_NAMESPACE_END
