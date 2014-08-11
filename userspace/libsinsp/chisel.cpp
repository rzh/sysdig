/*
Copyright (C) 2013-2014 Draios inc.

This file is part of sysdig.

sysdig is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

sysdig is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with sysdig.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <iostream>
#include <fstream>
#include <cctype>
#include <locale>
#ifndef _WIN32
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>
#endif
#include <third-party/tinydir.h>
#include <json/json.h>

#include "sinsp.h"
#include "sinsp_int.h"
#include "chisel.h"
#include "chisel_api.h"
#include "filter.h"
#include "filterchecks.h"

#ifdef HAS_CHISELS
#define HAS_LUA_CHISELS

#ifdef HAS_LUA_CHISELS

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}
#endif

extern vector<chiseldir_info>* g_chisel_dirs;
extern sinsp_filter_check_list g_filterlist;
extern sinsp_evttables g_infotables;

///////////////////////////////////////////////////////////////////////////////
// For Lua debugging
///////////////////////////////////////////////////////////////////////////////
#ifdef HAS_LUA_CHISELS
#if 0
//
// Useful when debugging the lua interface
//
void lua_stackdump(lua_State *L) 
{
	int i;
	int top = lua_gettop(L);
	for (i = 1; i <= top; i++) 
	{
		int t = lua_type(L, i);
		switch (t) 
		{

		case LUA_TSTRING:  // strings
			printf("`%s'", lua_tostring(L, i));
			break;

		case LUA_TBOOLEAN:  // booleans
			printf(lua_toboolean(L, i) ? "true" : "false");
			break;

		case LUA_TNUMBER:  // numbers
			printf("%g", lua_tonumber(L, i));
			break;

		default:  // other values
			printf("%s", lua_typename(L, t));
			break;
		}

		printf("  ");  // put a separator
	}

	printf("\n");  // end the listing
}
#endif

#if defined(_WIN32) && !defined(_WIN64)
//
// Essentially, there is no way to cleanly return an error from C++ to Lua in
// x86 windows, therefore we just print the error and exit
//
void sinsp_lua_error(lua_State *ls, string errorstr)
{
	cerr << errorstr << endl;
	exit(0);
}
#else
void sinsp_lua_error(lua_State *ls, string errorstr)
{
	luaL_error(ls, errorstr.c_str());
	ASSERT(false);
}
#endif

#endif

///////////////////////////////////////////////////////////////////////////////
// Lua callbacks
///////////////////////////////////////////////////////////////////////////////
#ifdef HAS_LUA_CHISELS

const static struct luaL_reg ll_sysdig [] = 
{
	{"set_filter", &lua_cbacks::set_global_filter},
	{"set_snaplen", &lua_cbacks::set_snaplen},
	{"set_output_format", &lua_cbacks::set_output_format},
	{"set_fatfile_dump_mode", &lua_cbacks::set_fatfile_dump_mode},
	{"is_live", &lua_cbacks::is_live},
	{"is_tty", &lua_cbacks::is_tty},
	{"get_filter", &lua_cbacks::get_filter},
	{"get_machine_info", &lua_cbacks::get_machine_info},
	{"get_thread_table", &lua_cbacks::get_thread_table},
	{"get_output_format", &lua_cbacks::get_output_format},
	{"get_evtsource_name", &lua_cbacks::get_evtsource_name},
	{"make_ts", &lua_cbacks::make_ts},
	{"run_sysdig", &lua_cbacks::run_sysdig},
	{NULL,NULL}
};

const static struct luaL_reg ll_chisel [] = 
{
	{"request_field", &lua_cbacks::request_field},
	{"set_filter", &lua_cbacks::set_filter},
	{"set_event_formatter", &lua_cbacks::set_event_formatter},
	{"set_interval_ns", &lua_cbacks::set_interval_ns},
	{"set_interval_s", &lua_cbacks::set_interval_s},
	{"exec", &lua_cbacks::exec},
	{NULL,NULL}
};

const static struct luaL_reg ll_evt [] = 
{
	{"field", &lua_cbacks::field},
	{"get_num", &lua_cbacks::get_num},
	{"get_ts", &lua_cbacks::get_ts},
	{"get_type", &lua_cbacks::get_type},
	{"get_cpuid", &lua_cbacks::get_cpuid},
	{NULL,NULL}
};
#endif // HAS_LUA_CHISELS

///////////////////////////////////////////////////////////////////////////////
// chiselinfo implementation
///////////////////////////////////////////////////////////////////////////////
chiselinfo::chiselinfo(sinsp* inspector)
{
	m_filter = NULL;
	m_formatter = NULL;
	m_dumper = NULL;
	m_inspector = inspector;
	m_has_nextrun_args = false;

#ifdef HAS_LUA_CHISELS
	m_callback_interval = 0;
#endif
}

chiselinfo::~chiselinfo()
{
	if(m_filter)
	{
		delete m_filter;
	}

	if(m_formatter)
	{
		delete m_formatter;
	}

	if(m_dumper)
	{
		delete m_dumper;
	}
}

void chiselinfo::init(string filterstr, string formatterstr)
{
	set_filter(filterstr);
	set_formatter(formatterstr);
}

void chiselinfo::set_filter(string filterstr)
{
	if(m_filter)
	{
		delete m_filter;
		m_filter = NULL;
	}

	if(filterstr != "")
	{
		m_filter = new sinsp_filter(m_inspector, filterstr);
	}
}

void chiselinfo::set_formatter(string formatterstr)
{
	if(m_formatter)
	{
		delete m_formatter;
		m_formatter = NULL;
	}

	if(formatterstr == "" || formatterstr == "default")
	{
		m_formatter = new sinsp_evt_formatter(m_inspector, DEFAULT_OUTPUT_STR);
	}
	else
	{
		m_formatter = new sinsp_evt_formatter(m_inspector, formatterstr);
	}
}

#ifdef HAS_LUA_CHISELS
void chiselinfo::set_callback_interval(uint64_t interval)
{
	m_callback_interval = interval;
}
#endif

///////////////////////////////////////////////////////////////////////////////
// chisel implementation
///////////////////////////////////////////////////////////////////////////////
sinsp_chisel::sinsp_chisel(sinsp* inspector, string filename)
{
	m_inspector = inspector;
	m_ls = NULL;
	m_lua_has_on_event = false;
	m_lua_is_first_evt = true;
	m_lua_cinfo = NULL;
	m_lua_last_interval_sample_time = 0;
	m_lua_last_interval_ts = 0;

	load(filename);
}

sinsp_chisel::~sinsp_chisel()
{
	free_lua_chisel();
}

void sinsp_chisel::free_lua_chisel()
{
#ifdef HAS_LUA_CHISELS
	if(m_ls)
	{
		lua_close(m_ls);
		m_ls = NULL;
	}

	for(uint32_t j = 0; j < m_allocated_fltchecks.size(); j++)
	{
		delete m_allocated_fltchecks[j];
	}
	m_allocated_fltchecks.clear();

	if(m_lua_cinfo != NULL)
	{
		delete m_lua_cinfo; 
		m_lua_cinfo = NULL;
	}

	m_lua_script_info.reset();
#endif
}

#ifdef HAS_LUA_CHISELS
void parse_lua_chisel_arg(lua_State *ls, OUT chisel_desc* cd)
{
	lua_pushnil(ls);
	string name;
	string type;
	string desc;
	bool optional = false;

	while(lua_next(ls, -2) != 0)
	{
		if(lua_isstring(ls, -1))
		{
			if(string(lua_tostring(ls, -2)) == "name")
			{
				name = lua_tostring(ls, -1);
			}
			else if(string(lua_tostring(ls, -2)) == "argtype")
			{
				type = lua_tostring(ls, -1);
			}
			else if(string(lua_tostring(ls, -2)) == "description")
			{
				desc = lua_tostring(ls, -1);
			}
		}
		else if(lua_isboolean(ls, -1))
		{
			if(string(lua_tostring(ls, -2)) == "optional")
			{
				optional = (lua_toboolean(ls, -1) != 0);
			}
		}
		else
		{
			sinsp_lua_error(ls, string(lua_tostring(ls, -2)) + " is not a string");
		}

		lua_pop(ls, 1);
	}

	cd->m_args.push_back(chiselarg_desc(name, type, desc, optional));
}

void parse_lua_chisel_args(lua_State *ls, OUT chisel_desc* cd)
{
	lua_pushnil(ls);

	while(lua_next(ls, -2) != 0)
	{
		if(lua_isstring(ls, -1))
		{
			printf("%s = %s\n", lua_tostring(ls, -2), lua_tostring(ls, -1));
			cd->m_description = lua_tostring(ls, -1);
		}
		else if(lua_istable(ls, -1))
		{
			parse_lua_chisel_arg(ls, cd);
		}
		else
		{
			sinsp_lua_error(ls, string(lua_tostring(ls, -2)) + " is not a string");
		}

		lua_pop(ls, 1);
	}
}

void sinsp_chisel::add_lua_package_path(lua_State* ls, const char* path)
{
	lua_getglobal(ls, "package");
	lua_getfield(ls, -1, "path"); 

	string cur_path = lua_tostring(ls, -1 );
	cur_path += ';';
	cur_path.append(path);
	lua_pop(ls, 1);

	lua_pushstring(ls, cur_path.c_str());
	lua_setfield(ls, -2, "path");
	lua_pop(ls, 1);
}
#endif

#ifdef HAS_LUA_CHISELS
// Initializes a lua chisel
bool sinsp_chisel::init_lua_chisel(chisel_desc &cd, string const &fpath)
{
	lua_State* ls = lua_open();
	luaL_openlibs(ls);

	//
	// Load our own lua libs
	//
	luaL_openlib(ls, "sysdig", ll_sysdig, 0);
	luaL_openlib(ls, "chisel", ll_chisel, 0);
	luaL_openlib(ls, "evt", ll_evt, 0);

	//
	// Add our chisel paths to package.path
	//
	for(vector<chiseldir_info>::const_iterator it = g_chisel_dirs->begin();
		it != g_chisel_dirs->end(); ++it)
	{
		string path(it->m_dir);
		path += "?.lua";
		add_lua_package_path(ls, path.c_str());
	}

	//
	// Load the script
	//
	if(luaL_loadfile(ls, fpath.c_str()) || lua_pcall(ls, 0, 0, 0))
	{
		goto failure;
	}

	//
	// Extract the description
	//
	lua_getglobal(ls, "description");
	if(!lua_isstring(ls, -1))
	{
		goto failure;
	}
	cd.m_description = lua_tostring(ls, -1);

	//
	// Extract the short description
	//
	lua_getglobal(ls, "short_description");
	if(!lua_isstring(ls, -1))
	{
		goto failure;
	}
	cd.m_shortdesc = lua_tostring(ls, -1);

	//
	// Extract the category
	//
	cd.m_category = "";
	lua_getglobal(ls, "category");
	if(lua_isstring(ls, -1))
	{
		cd.m_category = lua_tostring(ls, -1);
	}

	//
	// Extract the hidden flag and skip the chisel if it's set
	//
	lua_getglobal(ls, "hidden");
	if(lua_isboolean(ls, -1))
	{
		int sares = lua_toboolean(ls, -1);
		if(sares)
		{
			goto failure;
		}
	}

	//
	// Extract the args
	//
	lua_getglobal(ls, "args");
	try
	{
		parse_lua_chisel_args(ls, &cd);
	}
	catch(...)
	{
		goto failure;
	}
	return true;

failure:
	lua_close(ls);
	return false;
}
#endif

struct filename
{
    bool valid;
    string name;
    string ext;
};

static filename split_filename(string const &fname)
{
	filename res;
	string::size_type idx = fname.rfind('.');
	if(idx == std::string::npos)
	{
		res.valid = false;
	}
	else
	{
		res.valid = true;
		res.name = fname.substr(0, idx);
		res.ext = fname.substr(idx+1);
	}
	return res;
}

//
// 1. Iterates through the chisel files on disk (.sc and .lua)
// 2. Opens them and extracts the fields (name, description, etc)
// 3. Adds them to the chisel_descs vector.
//
void sinsp_chisel::get_chisel_list(vector<chisel_desc>* chisel_descs)
{
	for(vector<chiseldir_info>::const_iterator it = g_chisel_dirs->begin();
		it != g_chisel_dirs->end(); ++it)
	{
		if(string(it->m_dir).empty())
		{
			continue;
		}
		tinydir_dir dir;
		tinydir_open(&dir, it->m_dir);
		while(dir.has_next)
		{
			tinydir_file file;
			tinydir_readfile(&dir, &file);

			string fpath(file.path);
			bool add_to_vector = false;
			chisel_desc cd;

			filename fn = split_filename(string(file.name));
			if(fn.ext != "sc" && fn.ext != "lua")
			{
				goto next_file;
			}

			for(vector<chisel_desc>::const_iterator it_desc = chisel_descs->begin();
				it_desc != chisel_descs->end(); ++it_desc)
			{
				if(fn.name == it_desc->m_name)
				{
					goto next_file;
				}
			}
			cd.m_name = fn.name;
			
#ifdef HAS_LUA_CHISELS
			if(fn.ext == "lua")
			{
				add_to_vector = init_lua_chisel(cd, fpath);
			}
			
			if(add_to_vector)
			{
				chisel_descs->push_back(cd);
			}
#endif
next_file:
			tinydir_next(&dir);
		}
		tinydir_close(&dir);
	}
}

//
// If the function succeeds, is is initialized to point to the file.
// Otherwise, the return value is "false".
//
bool sinsp_chisel::openfile(string filename, OUT ifstream* is)
{
	uint32_t j;

	for(j = 0; j < g_chisel_dirs->size(); j++)
	{
		is->open(string(g_chisel_dirs->at(j).m_dir) + filename);
		if(is->is_open())
		{
			return true;
		}
	}

	return false;
}

void sinsp_chisel::load(string cmdstr)
{
	m_filename = cmdstr;
	trim(cmdstr);

	ifstream is;

	//
	// Try to open the file as is
	//
	if(!openfile(m_filename, &is))
	{
		//
		// Try to add the .sc extension
		//
		if(!openfile(m_filename + ".sc", &is))
		{
			if(!openfile(m_filename + ".lua", &is))
			{
				throw sinsp_exception("can't open file " + m_filename);
			}
		}
	}

	//
	// Bring the file into a string
	//
	string docstr((istreambuf_iterator<char>(is)),
		istreambuf_iterator<char>());

#ifdef HAS_LUA_CHISELS
	//
	// Rewind the stream
	//
	is.seekg(0);

	//
	// Load the file
	//
	std::istreambuf_iterator<char> eos;
	std::string scriptstr(std::istreambuf_iterator<char>(is), eos);

	//
	// Open the script
	//
	m_ls = lua_open();
 
	luaL_openlibs(m_ls);

	//
	// Load our own lua libs
	//
	luaL_openlib(m_ls, "sysdig", ll_sysdig, 0);
	luaL_openlib(m_ls, "chisel", ll_chisel, 0);
	luaL_openlib(m_ls, "evt", ll_evt, 0);

	//
	// Add our chisel paths to package.path
	//
	for(uint32_t j = 0; j < g_chisel_dirs->size(); j++)
	{
		string path(g_chisel_dirs->at(j).m_dir);
		path += "?.lua";
		add_lua_package_path(m_ls, path.c_str());
	}

	//
	// Load the script
	//
	if(luaL_loadstring(m_ls, scriptstr.c_str()) || lua_pcall(m_ls, 0, 0, 0)) 
	{
		throw sinsp_exception("Failed to load chisel " + 
			m_filename + ": " + lua_tostring(m_ls, -1));
	}

	//
	// Allocate the chisel context for the script
	//
	m_lua_cinfo = new chiselinfo(m_inspector);

	//
	// Set the context globals
	//
	lua_pushlightuserdata(m_ls, this);
	lua_setglobal(m_ls, "sichisel");

	//
	// Extract the args
	//
	lua_getglobal(m_ls, "args");
	if(!lua_istable(m_ls, -1))
	{
		throw sinsp_exception("Failed to load chisel " + 
			m_filename + ": args table missing");
	}

	try
	{
		parse_lua_chisel_args(m_ls, &m_lua_script_info);
	}
	catch(sinsp_exception& e)
	{
		throw e;
	}

	//
	// Check if the script has an on_event
	//
	lua_getglobal(m_ls, "on_event");
	if(lua_isfunction(m_ls, -1))
	{
		m_lua_has_on_event = true;
		lua_pop(m_ls, 1);
	}
#endif

	is.close();
}

uint32_t sinsp_chisel::get_n_args()
{
	ASSERT(m_ls);

#ifdef HAS_LUA_CHISELS
	return (uint32_t)m_lua_script_info.m_args.size();
#else
	return 0;
#endif
}

uint32_t sinsp_chisel::get_n_optional_args()
{
	uint32_t j;
	uint32_t res = 0;

	for(j = 0; j < m_lua_script_info.m_args.size(); j++)
	{
		if(m_lua_script_info.m_args[j].m_optional)
		{
			res++;
		}
	}

	return res;
}

uint32_t sinsp_chisel::get_n_required_args()
{
	uint32_t j;
	uint32_t res = 0;

	for(j = 0; j < m_lua_script_info.m_args.size(); j++)
	{
		if(!m_lua_script_info.m_args[j].m_optional)
		{
			res++;
		}
	}

	return res;
}

void sinsp_chisel::set_args(string args)
{
#ifdef HAS_LUA_CHISELS
	uint32_t j;
	uint32_t n_required_args = get_n_required_args();
	uint32_t n_optional_args = get_n_optional_args();

	ASSERT(m_ls);

	//
	// Split the argument string into tokens
	//
	uint32_t token_begin = 0;
	bool inquotes = false;
	uint32_t quote_correction = 0;

	trim(args);

	if(args.size() != 0)
	{
		for(j = 0; j < args.size(); j++)
		{
			if(args[j] == ' ' && !inquotes)
			{
				m_argvals.push_back(args.substr(token_begin, j - quote_correction - token_begin));
				token_begin = j + 1;
				quote_correction = 0;
			}
			else if(args[j] == '\'' || args[j] == '`')
			{
				if(inquotes)
				{
					quote_correction = 1;
					inquotes = false;
				}			
				else {
					token_begin++;
					inquotes = true;
				}			
			}
		}
	
		if(inquotes)
		{
			throw sinsp_exception("corrupted parameters for chisel " + m_filename);
		}

		m_argvals.push_back(args.substr(token_begin, j - quote_correction - token_begin));
	}

	//
	// Validate the arguments
	//
	if(m_argvals.size() < n_required_args)
	{
		throw sinsp_exception("wrong number of parameters for chisel " + m_filename +
			", " + to_string((long long int)n_required_args) + " required, " + 
			to_string((long long int)m_argvals.size()) + " given");
	}
	else if(m_argvals.size() > n_optional_args + n_required_args)
	{
		throw sinsp_exception("too many parameters for chisel " + m_filename +
			", " + to_string((long long int)(n_required_args)) + " required, " +
			to_string((long long int)(n_optional_args)) + " optional, " +
                        to_string((long long int)m_argvals.size()) + " given");
	}

	//
	// Push the arguments
	//
	for(j = 0; j < m_argvals.size(); j++)
	{
		lua_getglobal(m_ls, "on_set_arg");
		if(!lua_isfunction(m_ls, -1))
		{
			lua_pop(m_ls, 1);
			throw sinsp_exception("chisel " + m_filename + " misses a set_arg() function.");
		}

		lua_pushstring(m_ls, m_lua_script_info.m_args[j].m_name.c_str()); 
		lua_pushstring(m_ls, m_argvals[j].c_str());

		//
		// call get_info()
		//
		if(lua_pcall(m_ls, 2, 1, 0) != 0) 
		{
			throw sinsp_exception(m_filename + " chisel error: " + lua_tostring(m_ls, -1));
		}

		if(!lua_isboolean(m_ls, -1)) 
		{
			throw sinsp_exception(m_filename + " chisel error: wrong set_arg() return value.");
		}

		int sares = lua_toboolean(m_ls, -1);

		if(!sares)
		{
			throw sinsp_exception("set_arg() for chisel " + m_filename + " failed.");
		}

		lua_pop(m_ls, 1);
	}
#endif
}

void sinsp_chisel::on_init()
{
	//
	// Done with the arguments, call init()
	//
	lua_getglobal(m_ls, "on_init");

	if(!lua_isfunction(m_ls, -1)) 
	{
		//
		// No on_init. 
		// That's ok. Just return.
		//
		return;
	}

	if(lua_pcall(m_ls, 0, 1, 0) != 0) 
	{
		//
		// Exception running init
		//
		const char* lerr = lua_tostring(m_ls, -1);
		string err = m_filename + ": error in init(): " + lerr;
		throw sinsp_exception(err);
	}

	if(m_new_chisel_to_exec == "")
	{
		if(!lua_isboolean(m_ls, -1)) 
		{
			throw sinsp_exception(m_filename + " chisel error: wrong init() return value.");
		}

		if(!lua_toboolean(m_ls, -1))
		{
			throw sinsp_exception("init() for chisel " + m_filename + " failed.");
		}
	}

	lua_pop(m_ls, 1);

	//
	// If the chisel called chisel.exec(), free this chisel and load the new one
	//
	if(m_new_chisel_to_exec != "")
	{
		free_lua_chisel();
		load(m_new_chisel_to_exec);
		m_new_chisel_to_exec = "";

		string args;
		for(uint32_t j = 0; j < m_argvals.size(); j++)
		{
			if(m_argvals[j].find(" ") == string::npos)
			{
				args += m_argvals[j];
			}
			else
			{
				args += string("'") + m_argvals[j] + "'";
			}

			if(j < m_argvals.size() - 1)
			{
				args += " ";
			}
		}

		m_argvals.clear();
		set_args(args);

		on_init();
	}
}

void sinsp_chisel::first_event_inits(sinsp_evt* evt)
{
	lua_pushlightuserdata(m_ls, evt);
	lua_setglobal(m_ls, "sievt");

	uint64_t ts = evt->get_ts();
	if(m_lua_cinfo->m_callback_interval != 0)
	{
		m_lua_last_interval_sample_time = ts - ts % m_lua_cinfo->m_callback_interval;
	}

	m_lua_is_first_evt = false;
}

bool sinsp_chisel::run(sinsp_evt* evt)
{
#ifdef HAS_LUA_CHISELS
	string line;

	ASSERT(m_ls);

	//
	// If this is the first event, put the event pointer on the stack.
	// We assume that the event pointer will never change.
	//
	if(m_lua_is_first_evt)
	{
		first_event_inits(evt);
	}

	//
	// If there is a timeout callback, see if it's time to call it
	//
	do_timeout(evt);

	//
	// If there is a filter, run it
	//
	if(m_lua_cinfo->m_filter != NULL)
	{
		if(!m_lua_cinfo->m_filter->run(evt))
		{
			return false;
		}
	}

	//
	// If the script has the on_event callback, call it
	//
	if(m_lua_has_on_event)
	{
		lua_getglobal(m_ls, "on_event");
			
		if(lua_pcall(m_ls, 0, 1, 0) != 0) 
		{
			throw sinsp_exception(m_filename + " chisel error: " + lua_tostring(m_ls, -1));
		}

		int oeres = lua_toboolean(m_ls, -1);
		lua_pop(m_ls, 1);

		if(oeres == false)
		{
			return false;
		}
	}

	//
	// If the script has a formatter, run it
	//
	if(m_lua_cinfo->m_formatter != NULL)
	{
		if(m_lua_cinfo->m_formatter->tostring(evt, &line))
		{
			cout << line << endl;
		}
	}

	return true;
#endif
}

void sinsp_chisel::do_timeout(sinsp_evt* evt)
{
	if(m_lua_is_first_evt)
	{
		first_event_inits(evt);
	}

	if(m_lua_cinfo->m_callback_interval != 0)
	{
		uint64_t ts = evt->get_ts();
		uint64_t sample_time = ts - ts % m_lua_cinfo->m_callback_interval;

		if(sample_time != m_lua_last_interval_sample_time)
		{
			int64_t delta = 0;

			if(m_lua_last_interval_ts != 0)
			{
				delta = ts - m_lua_last_interval_ts;
				ASSERT(delta > 0);
			}

			lua_getglobal(m_ls, "on_interval");
			
			lua_pushnumber(m_ls, (double)(ts / 1000000000)); 
			lua_pushnumber(m_ls, (double)(ts % 1000000000)); 
			lua_pushnumber(m_ls, (double)delta); 

			if(lua_pcall(m_ls, 3, 1, 0) != 0) 
			{
				throw sinsp_exception(m_filename + " chisel error: calling on_interval() failed:" + lua_tostring(m_ls, -1));
			}
	
			int oeres = lua_toboolean(m_ls, -1);
			lua_pop(m_ls, 1);

			if(oeres == false)
			{
				throw sinsp_exception("execution terminated by the " + m_filename + " chisel");
			}
	
			m_lua_last_interval_sample_time = sample_time;
			m_lua_last_interval_ts = ts;
		}
	}
}

void sinsp_chisel::on_capture_start()
{
#ifdef HAS_LUA_CHISELS
	lua_getglobal(m_ls, "on_capture_start");
			
	if(lua_isfunction(m_ls, -1))
	{
		if(lua_pcall(m_ls, 0, 1, 0) != 0) 
		{
			throw sinsp_exception(m_filename + " chisel error: " + lua_tostring(m_ls, -1));
		}

		if(!lua_isboolean(m_ls, -1)) 
		{
			throw sinsp_exception(m_filename + " chisel error: wrong on_capture_start() return value. Boolean expected.");
		}

		if(!lua_toboolean(m_ls, -1))
		{
			throw sinsp_exception("init() for chisel " + m_filename + " failed.");
		}

		lua_pop(m_ls, 1);
	}
#endif // HAS_LUA_CHISELS
}

void sinsp_chisel::on_capture_end()
{
#ifdef HAS_LUA_CHISELS
	lua_getglobal(m_ls, "on_capture_end");

	if(lua_isfunction(m_ls, -1))
	{
		uint64_t ts = m_inspector->m_firstevent_ts;
		uint64_t te = m_inspector->m_lastevent_ts;
		int64_t delta = te - ts;

		lua_pushnumber(m_ls, (double)(te / 1000000000)); 
		lua_pushnumber(m_ls, (double)(te % 1000000000)); 
		lua_pushnumber(m_ls, (double)delta);

		if(lua_pcall(m_ls, 3, 0, 0) != 0) 
		{
			string err = lua_tostring(m_ls, -1);
			throw sinsp_exception(m_filename + " chisel error: " + err);
		}

		lua_pop(m_ls, 1);
	}
#endif // HAS_LUA_CHISELS
}

bool sinsp_chisel::get_nextrun_args(OUT string* args)
{
	ASSERT(m_lua_cinfo != NULL);

	*args = m_lua_cinfo->m_nextrun_args;
	return m_lua_cinfo->m_has_nextrun_args;
}

#endif // HAS_CHISELS
