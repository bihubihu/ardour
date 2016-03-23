/*
    Copyright (C) 2016 Robin Gareus <robin@gareus.org>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/

#ifdef PLATFORM_WINDOWS
#define random() rand()
#endif

#ifdef WAF_BUILD
#include "gtk2ardour-config.h"
#endif

#include <glibmm/fileutils.h>
#include <gtkmm/messagedialog.h>

#include "pbd/basename.h"
#include "pbd/file_utils.h"
#include "pbd/md5.h"

#include "gtkmm2ext/gtk_ui.h"
#include "gtkmm2ext/utils.h"
#include "gtkmm2ext/window_title.h"

#include "ardour/luabindings.h"
#include "LuaBridge/LuaBridge.h"

#include "ardour_ui.h"
#include "gui_thread.h"
#include "luainstance.h"
#include "luawindow.h"
#include "public_editor.h"
#include "tooltips.h"
#include "utils.h"

#include "i18n.h"

using namespace ARDOUR;
using namespace ARDOUR_UI_UTILS;
using namespace PBD;
using namespace Gtk;
using namespace Glib;
using namespace Gtkmm2ext;
using namespace std;


inline LuaWindow::BufferFlags operator| (const LuaWindow::BufferFlags& a, const LuaWindow::BufferFlags& b) {
	return static_cast<LuaWindow::BufferFlags> (static_cast <int>(a) | static_cast<int> (b));
}

inline LuaWindow::BufferFlags operator|= (LuaWindow::BufferFlags& a, const LuaWindow::BufferFlags& b) {
	return a = static_cast<LuaWindow::BufferFlags> (static_cast <int>(a) | static_cast<int> (b));
}

inline LuaWindow::BufferFlags operator&= (LuaWindow::BufferFlags& a, const LuaWindow::BufferFlags& b) {
	return a = static_cast<LuaWindow::BufferFlags> (static_cast <int>(a) & static_cast<int> (b));
}

LuaWindow* LuaWindow::_instance = 0;

LuaWindow*
LuaWindow::instance ()
{
	if (!_instance) {
		_instance  = new LuaWindow;
	}

	return _instance;
}

LuaWindow::LuaWindow ()
	: Window (Gtk::WINDOW_TOPLEVEL)
	, VisibilityTracker (*((Gtk::Window*) this))
	, _visible (false)
	, _menu_scratch (0)
	, _menu_snippet (0)
	, _menu_actions (0)
	, _btn_run (_("Run"))
	, _btn_clear (_("Clear Outtput"))
	, _btn_open (_("Import"))
	, _btn_save (_("Save"))
	, _btn_delete (_("Delete"))
	, _current_buffer ()
{
	set_name ("Lua");

	update_title ();
	set_wmclass (X_("ardour_mixer"), PROGRAM_NAME);

	script_select.disable_scrolling ();

	set_border_width (0);

	outtext.set_editable (false);
	outtext.set_wrap_mode (Gtk::WRAP_WORD);
	outtext.set_cursor_visible (false);

	signal_delete_event().connect (sigc::mem_fun (*this, &LuaWindow::hide_window));
	signal_configure_event().connect (sigc::mem_fun (*ARDOUR_UI::instance(), &ARDOUR_UI::configure_handler));

	_btn_run.signal_clicked.connect (sigc::mem_fun(*this, &LuaWindow::run_script));
	_btn_clear.signal_clicked.connect (sigc::mem_fun(*this, &LuaWindow::clear_output));
	_btn_open.signal_clicked.connect (sigc::mem_fun(*this, &LuaWindow::import_script));
	_btn_save.signal_clicked.connect (sigc::mem_fun(*this, &LuaWindow::save_script));
	_btn_delete.signal_clicked.connect (sigc::mem_fun(*this, &LuaWindow::delete_script));

	_btn_open.set_sensitive (false); // TODO
	_btn_save.set_sensitive (false);
	_btn_delete.set_sensitive (false); // TODO

	// layout

	Gtk::ScrolledWindow *scrollin = manage (new Gtk::ScrolledWindow);
	scrollin->set_policy (Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
	scrollin->add (entry);
	scrollout.set_policy (Gtk::POLICY_NEVER, Gtk::POLICY_ALWAYS);
	scrollout.add (outtext);

	Gtk::HBox *hbox = manage (new HBox());

	hbox->pack_start (_btn_run, false, false, 2);
	hbox->pack_start (_btn_clear, false, false, 2);
	hbox->pack_start (_btn_open, false, false, 2);
	hbox->pack_start (_btn_save, false, false, 2);
	hbox->pack_start (_btn_delete, false, false, 2);
	hbox->pack_start (script_select, false, false, 2);

	Gtk::VBox *vbox = manage (new VBox());
	vbox->pack_start (*scrollin, true, true, 0);
	vbox->pack_start (*hbox, false, false, 2);

	Gtk::VPaned *vpane = manage (new Gtk::VPaned ());
	vpane->pack1 (*vbox, true, false);
	vpane->pack2 (scrollout, false, true);

	vpane->show_all ();
	add (*vpane);
	set_size_request (640, 480); // XXX

	lua.Print.connect (sigc::mem_fun (*this, &LuaWindow::append_text));

	lua_State* L = lua.getState();
	LuaInstance::register_classes (L);
	luabridge::push <PublicEditor *> (L, &PublicEditor::instance());
	lua_setglobal (L, "Editor");

	ARDOUR_UI_UTILS::set_tooltip (script_select, _("Select Editor Buffer"));

	setup_buffers ();
	LuaScripting::instance().scripts_changed.connect (*this, invalidator (*this), boost::bind (&LuaWindow::refresh_scriptlist, this), gui_context());

	Glib::RefPtr<Gtk::TextBuffer> tb (entry.get_buffer());

	_script_changed_connection = tb->signal_changed().connect (sigc::mem_fun(*this, &LuaWindow::script_changed));
}

LuaWindow::~LuaWindow ()
{
}

void
LuaWindow::show_window ()
{
	present();
	_visible = true;
}

bool
LuaWindow::hide_window (GdkEventAny *ev)
{
	if (!_visible) return 0;
	_visible = false;
	return just_hide_it (ev, static_cast<Gtk::Window *>(this));
}

void LuaWindow::set_session (Session* s)
{
	SessionHandlePtr::set_session (s);
	if (!_session) {
		return;
	}

	update_title ();
	_session->DirtyChanged.connect (_session_connections, invalidator (*this), boost::bind (&LuaWindow::update_title, this), gui_context());

	// expose "Session" point directly
	lua_State* L = lua.getState();
	LuaBindings::set_session (L, _session);
}

void
LuaWindow::session_going_away ()
{
	ENSURE_GUI_THREAD (*this, &LuaWindow::session_going_away);
	lua.do_command ("collectgarbage();");
	//TODO: re-init lua-engine (drop all references) ??

	SessionHandlePtr::session_going_away ();
	_session = 0;
	update_title ();

	lua_State* L = lua.getState();
	LuaBindings::set_session (L, _session);
}

void
LuaWindow::update_title ()
{
	if (_session) {
		string n;

		if (_session->snap_name() != _session->name()) {
			n = _session->snap_name ();
		} else {
			n = _session->name ();
		}

		if (_session->dirty ()) {
			n = "*" + n;
		}

		WindowTitle title (n);
		title += S_("Window|Lua");
		title += Glib::get_application_name ();
		set_title (title.get_string());

	} else {
		WindowTitle title (S_("Window|Lua"));
		title += Glib::get_application_name ();
		set_title (title.get_string());
	}
}

void
LuaWindow::scroll_to_bottom ()
{
	Gtk::Adjustment *adj;
	adj = scrollout.get_vadjustment();
	adj->set_value (MAX(0,(adj->get_upper() - adj->get_page_size())));
}

void
LuaWindow::run_script ()
{
	Glib::RefPtr<Gtk::TextBuffer> tb (entry.get_buffer());
	std::string script = tb->get_text();
	const std::string& bytecode = LuaScripting::get_factory_bytecode (script);
	if (bytecode.empty()) {
		// plain script or faulty script -- run directly
		try {
			lua.do_command ("function ardour () end");
			if (0 == lua.do_command (script)) {
				append_text ("> OK");
			}
		} catch (luabridge::LuaException const& e) {
			append_text (string_compose (_("LuaException: %1"), e.what()));
		}
	} else {
		// script with factory method
		try {
			lua_State* L = lua.getState();
			lua.do_command ("function ardour () end");

			LuaScriptParamList args = LuaScriptParams::script_params (script, "action_param", false);
			luabridge::LuaRef tbl_arg (luabridge::newTable(L));
			LuaScriptParams::params_to_ref (&tbl_arg, args);
			lua.do_command (script); // register "factory"
			luabridge::LuaRef lua_factory = luabridge::getGlobal (L, "factory");
			if (lua_factory.isFunction()) {
				lua_factory(tbl_arg)();
			}
			lua.do_command ("factory = nil;");
		} catch (luabridge::LuaException const& e) {
			append_text (string_compose (_("LuaException: %1"), e.what()));
		}
	}
}

void
LuaWindow::append_text (std::string s)
{
	Glib::RefPtr<Gtk::TextBuffer> tb (outtext.get_buffer());
	tb->insert (tb->end(), s + "\n");
	scroll_to_bottom ();
}

void
LuaWindow::clear_output ()
{
	Glib::RefPtr<Gtk::TextBuffer> tb (outtext.get_buffer());
	tb->set_text ("");
}

void
LuaWindow::new_script ()
{
#if 0
	Glib::RefPtr<Gtk::TextBuffer> tb (entry.get_buffer());
	tb->set_text ("");
#endif
}

void
LuaWindow::delete_script ()
{
#if 0
	Glib::RefPtr<Gtk::TextBuffer> tb (entry.get_buffer());
	tb->set_text ("");
#endif
}

void
LuaWindow::import_script ()
{
}

void
LuaWindow::save_script ()
{
	Glib::RefPtr<Gtk::TextBuffer> tb (entry.get_buffer());
	std::string script = tb->get_text();
	std::string msg = "Unknown error";

	std::string path;
	LuaScriptInfoPtr lsi = LuaScripting::script_info (script);
	ScriptBuffer & sb (*_current_buffer);

	assert (sb.flags & Buffer_Dirty);

	// 1) check if it has a valid header and factory
	const std::string& bytecode = LuaScripting::get_factory_bytecode (script);
	if (bytecode.empty()) {
		msg = _("Missing script header.\nThe script requires an '{ardour}' info table and a 'factory' function.");
		goto errorout;
	}

	if (!LuaScripting::try_compile (script, LuaScriptParams::script_params (script, "action_param", false))) {
		msg = _("Script fails to compile.");
		goto errorout;
	}

	// 2) check script name & type
	lsi = LuaScripting::script_info (script);
	if (!lsi) {
		msg = _("Invalid or missing script-name or script-type.");
		goto errorout;
	}

	if (lsi->type != LuaScriptInfo::Snippet && lsi->type != LuaScriptInfo::EditorAction) {
		msg = _("Invalid script-type.\nValid types are 'EditorAction' and 'Snippet'.");
		goto errorout;
	}

	// 3) if there's already a writable file,...
	if ((sb.flags & Buffer_HasFile) && !(sb.flags & Buffer_ReadOnly)) {
		try {
			Glib::file_set_contents (sb.path, script);
			sb.flags &= BufferFlags(~Buffer_Dirty);
			update_gui_state (); // XXX here?
			append_text (X_("> ") + string_compose (_("Saved as %1"), sb.path));
			return; // OK
		} catch (Glib::FileError e) {
			msg = string_compose (_("Error saving file: %1"), e.what());
			goto errorout;
		}
	}

	// 4) check if the name is unique for the given type; locally at least
	if (true /*sb.flags & Buffer_HasFile*/) {
		LuaScriptList& lsl (LuaScripting::instance ().scripts (lsi->type));
		for (LuaScriptList::const_iterator s = lsl.begin(); s != lsl.end(); ++s) {
			if ((*s)->name == lsi->name) {
				msg = string_compose (_("Script with given name '%1' already exists.\nUse a different name in the descriptor."), lsi->name);
				goto errorout;
			}
		}
	}

	// 5) construct filename -- TODO ask user for name, ask to replace file.
	do {
		char buf[80];
		time_t t = time(0);
		struct tm * timeinfo = localtime (&t);
		strftime (buf, sizeof(buf), "%s%d", timeinfo);
		sprintf (buf, "%s%d", buf, random ()); // is this valid?
		MD5 md5;
		std::string fn = md5.digestString (buf);

		switch (lsi->type) {
			case LuaScriptInfo::EditorAction:
				fn = "a_" + fn;
				break;
			case LuaScriptInfo::Snippet:
				fn = "s_" + fn;
				break;
			default:
				break;
		}
		path = Glib::build_filename (LuaScripting::user_script_dir (), fn.substr(0, 11) + ".lua");
	} while (Glib::file_test (path, Glib::FILE_TEST_EXISTS));

	try {
		Glib::file_set_contents (path, script);
		sb.path = path;
		sb.flags |= Buffer_HasFile;
		sb.flags &= BufferFlags(~Buffer_Dirty);
		update_gui_state (); // XXX here?
		LuaScripting::instance().refresh (true);
		append_text (X_("> ") + string_compose (_("Saved as %1"), path));
		return; // OK
	} catch (Glib::FileError e) {
		msg = string_compose (_("Error saving file: %1"), e.what());
		goto errorout;
	}

errorout:
		MessageDialog am (msg);
		am.run ();
}

void
LuaWindow::setup_buffers ()
{
	if (script_buffers.size() > 0) {
		return;
	}
	script_buffers.push_back (ScriptBufferPtr (new LuaWindow::ScriptBuffer("#1")));
	script_buffers.push_back (ScriptBufferPtr (new LuaWindow::ScriptBuffer("#2"))); // XXX
	_current_buffer = script_buffers.front();

	refresh_scriptlist ();
	update_gui_state ();
}

uint32_t
LuaWindow::count_scratch_buffers () const
{
	return 0;
}

void
LuaWindow::refresh_scriptlist ()
{
	for (ScriptBufferList::iterator i = script_buffers.begin (); i != script_buffers.end ();) {
		if ((*i)->flags & Buffer_Scratch) {
			++i;
			continue;
		}
		i = script_buffers.erase (i);
	}
	LuaScriptList& lsa (LuaScripting::instance ().scripts (LuaScriptInfo::EditorAction));
	for (LuaScriptList::const_iterator s = lsa.begin(); s != lsa.end(); ++s) {
		script_buffers.push_back (ScriptBufferPtr ( new LuaWindow::ScriptBuffer(*s)));
	}

	LuaScriptList& lss (LuaScripting::instance ().scripts (LuaScriptInfo::Snippet));
	for (LuaScriptList::const_iterator s = lss.begin(); s != lss.end(); ++s) {
		script_buffers.push_back (ScriptBufferPtr ( new LuaWindow::ScriptBuffer(*s)));
	}
	rebuild_menu ();
}

void
LuaWindow::rebuild_menu ()
{
	using namespace Menu_Helpers;

	_menu_scratch = manage (new Menu);
	_menu_snippet = manage (new Menu);
	_menu_actions = manage (new Menu);

	MenuList& items_scratch (_menu_scratch->items());
	MenuList& items_snippet (_menu_snippet->items());
	MenuList& items_actions (_menu_actions->items());

	{
		Menu_Helpers::MenuElem elem = Gtk::Menu_Helpers::MenuElem(_("New"),
				sigc::mem_fun(*this, &LuaWindow::new_script));
		items_scratch.push_back(elem);
	}

	for (ScriptBufferList::const_iterator i = script_buffers.begin (); i != script_buffers.end (); ++i) {
		Menu_Helpers::MenuElem elem = Gtk::Menu_Helpers::MenuElem((*i)->name,
				sigc::bind(sigc::mem_fun(*this, &LuaWindow::script_selection_changed), (*i)));

		if ((*i)->flags & Buffer_Scratch) {
			items_scratch.push_back(elem);
		}
		else if ((*i)->type == LuaScriptInfo::EditorAction) {
				items_actions.push_back(elem);
		}
		else if ((*i)->type == LuaScriptInfo::Snippet) {
				items_snippet.push_back(elem);
		}
	}

	script_select.clear_items ();
	script_select.AddMenuElem (Menu_Helpers::MenuElem ("Scratch", *_menu_scratch));
	script_select.AddMenuElem (Menu_Helpers::MenuElem ("Snippets", *_menu_snippet));
	script_select.AddMenuElem (Menu_Helpers::MenuElem ("Actions", *_menu_actions));
}

void
LuaWindow::script_selection_changed (ScriptBufferPtr n)
{
	if (n == _current_buffer) {
		return;
	}

	Glib::RefPtr<Gtk::TextBuffer> tb (entry.get_buffer());
	_current_buffer->script = tb->get_text();

	if (!(n->flags & Buffer_Valid)) {
		if (!n->load()) {
			append_text ("! Failed to load buffer.");
		}
	}

	if (n->flags & Buffer_Valid) {
		_current_buffer = n;
		_script_changed_connection.block ();
		tb->set_text (n->script);
		_script_changed_connection.unblock ();
	} else {
		append_text ("! Failed to switch buffer.");
	}
	update_gui_state ();
}

void
LuaWindow::update_gui_state ()
{
	const ScriptBuffer & sb (*_current_buffer);
	std::string name;
	if (sb.flags & Buffer_Scratch) {
		name = string_compose (_("Scratch Buffer %1"), sb.name);
	} else if (sb.type == LuaScriptInfo::EditorAction) {
		name = string_compose (_("Action: '%1'"), sb.name);
	} else if (sb.type == LuaScriptInfo::Snippet) {
		name = string_compose (_("Snippet: %1"), sb.name);
	} else {
		cerr << "Invalid Script type\n";
		assert (0);
		return;
	}
	if (sb.flags & Buffer_Dirty) {
		name += " *";
	}
	script_select.set_text(name);

	_btn_save.set_sensitive (sb.flags & Buffer_Dirty);
	_btn_delete.set_sensitive (sb.flags & Buffer_Scratch); // TODO allow to remove user-scripts
}

void
LuaWindow::script_changed () {
	if (_current_buffer->flags & Buffer_Dirty) {
		return;
	}
	_current_buffer->flags |= Buffer_Dirty;
	update_gui_state ();
}

LuaWindow::ScriptBuffer::ScriptBuffer (const std::string& n)
	: name (n)
	, flags (Buffer_Scratch | Buffer_Valid)
{
}

LuaWindow::ScriptBuffer::ScriptBuffer (LuaScriptInfoPtr p)
	: name (p->name)
	, path (p->path)
	, flags (Buffer_HasFile)
	, type (p->type)
{
	if (!PBD::exists_and_writable (path)) {
		flags |= Buffer_ReadOnly;
	}
}

#if 0
LuaWindow::ScriptBuffer::ScriptBuffer (const ScriptBuffer& other)
	: script (other.script)
	, name (other.name)
	, path (other.path)
	, flags (other.flags)
	, type (other.type)
{
}
#endif

LuaWindow::ScriptBuffer::~ScriptBuffer ()
{
}

bool
LuaWindow::ScriptBuffer::load ()
{
	if (!(flags & Buffer_HasFile)) return false;
	if (flags & Buffer_Valid) return true;
	try {
		script = Glib::file_get_contents (path);
		flags |= Buffer_Valid;
	} catch (Glib::FileError e) {
		return false;
	}
	return true;
}
