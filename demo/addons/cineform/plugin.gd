# SPDX-License-Identifier: Apache-2.0 OR MIT
@tool
extends EditorPlugin

# The writer registers itself from the GDExtension entry point, so this plugin adds no node
# and no dock. It exists so the addon appears in Project Settings under Plugins, which is
# where a user looks first when a recording did not produce the file they expected.

func _enter_tree() -> void:
	if not ClassDB.class_exists("MovieWriterCineForm"):
		push_error("CineForm: the GDExtension did not load. Check addons/cineform/bin.")
