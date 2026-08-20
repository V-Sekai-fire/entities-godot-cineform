// SPDX-License-Identifier: Apache-2.0 OR MIT
#include "register_types.h"
#include "movie_writer_cineform.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

// Held for the process lifetime. MovieWriter::add_writer does not take a reference, so a
// local would be freed the moment this function returns and Godot would call into it later.
static MovieWriterCineForm *writer = nullptr;

// Defined here so they exist in project.godot and in the editor, rather than being read from
// the environment. A setting nobody can see is a setting nobody sets.
static void _define_setting(const String &p_name, const Variant &p_default,
		PropertyHint p_hint = PROPERTY_HINT_NONE, const String &p_hint_string = String()) {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps->has_setting(p_name)) {
		ps->set_setting(p_name, p_default);
	}
	ps->set_initial_value(p_name, p_default);
	Dictionary info;
	info["name"] = p_name;
	info["type"] = p_default.get_type();
	info["hint"] = p_hint;
	info["hint_string"] = p_hint_string;
	ps->add_property_info(info);
}

static void _define_settings() {
	_define_setting("cineform/quality", 2, PROPERTY_HINT_ENUM,
			"Low,Medium,High,Filmscan1,Filmscan2,Filmscan3");
	_define_setting("cineform/thread_count", 0);
	_define_setting("cineform/keep_alpha", false);
	_define_setting("cineform/hdr", false);
	_define_setting("cineform/flip_in_codec", false);
	// Fastest, and the encoder reads Godot's RGBA as BGRA, so red and blue swap.
	_define_setting("cineform/zero_copy", false);
}

void initialize_cineform_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	GDREGISTER_CLASS(MovieWriterCineForm);
	_define_settings();
	writer = memnew(MovieWriterCineForm);
	MovieWriter::add_writer(writer);
}

void uninitialize_cineform_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	if (writer) {
		memdelete(writer);
		writer = nullptr;
	}
}

extern "C" {
GDExtensionBool GDE_EXPORT cineform_library_init(
		GDExtensionInterfaceGetProcAddress p_get_proc_address,
		const GDExtensionClassLibraryPtr p_library,
		GDExtensionInitialization *r_initialization) {
	GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);
	init_obj.register_initializer(initialize_cineform_module);
	init_obj.register_terminator(uninitialize_cineform_module);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
	return init_obj.init();
}
}
