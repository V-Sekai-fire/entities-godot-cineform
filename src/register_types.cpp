// SPDX-License-Identifier: Apache-2.0 OR MIT
#include "register_types.h"
#include "movie_writer_cineform.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

// Held for the process lifetime. MovieWriter::add_writer does not take a reference, so a
// local would be freed the moment this function returns and Godot would call into it later.
static MovieWriterCineForm *writer = nullptr;

void initialize_cineform_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	GDREGISTER_CLASS(MovieWriterCineForm);
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
