// SPDX-License-Identifier: Apache-2.0 OR MIT
#ifndef MOVIE_WRITER_CINEFORM_H
#define MOVIE_WRITER_CINEFORM_H

#include <godot_cpp/classes/movie_writer.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <cstdio>
#include <vector>

#include "CFHDEncoder.h"

namespace godot {

// A Godot Movie Maker writer that encodes CineForm.
//
// Registered with MovieWriter::add_writer at startup, this claims a file extension and
// receives every rendered frame. Godot drives it: _write_begin once, _write_frame for each
// frame, _write_end once.
//
// The virtual signatures below come from godot-cpp's extension_api-4-5.json, and they are
// byte for byte the same in extension_api-4-3.json.
class MovieWriterCineForm : public MovieWriter {
	GDCLASS(MovieWriterCineForm, MovieWriter)

	CFHD_EncoderPoolRef pool = nullptr;
	CFHD_MetadataRef metadata = nullptr;
	std::FILE *avi = nullptr;

	uint32_t frame_index = 0;
	uint32_t frames_written = 0;
	uint32_t queued = 0;
	Vector2i size;
	uint32_t frame_rate = 0;
	int thread_count = 0;
	int quality_index = 0;
	bool keep_alpha = false;

	// Godot hands us RGBA8 top down. CineForm's BGRA input wants the rows the other way and
	// the channels swapped, so this is where that happens. Doing it in place on Godot's
	// buffer would mutate a frame the engine still owns.
	std::vector<uint8_t> staging;

	// AVI index. Every sample offset and size, so the idx1 chunk can be written at the end.
	struct Entry {
		uint32_t offset;
		uint32_t size;
	};
	std::vector<Entry> index;
	long movi_start = 0;
	long riff_size_at = 0;
	long movi_size_at = 0;
	long frames_field_at = 0;
	long stream_len_at = 0;

	void drain(bool block);
	void write_sample(const void *data, size_t len);
	void begin_avi(const String &path);
	void finish_avi();

protected:
	static void _bind_methods();

public:
	MovieWriterCineForm();
	~MovieWriterCineForm();

	virtual bool _handles_file(const String &p_path) const override;
	// uint32_t, not int64_t. extension_api.json calls this `int`, and godot-cpp maps it to
	// uint32_t here. The JSON type names describe the Variant type, not the C++ one, so the
	// generated header is the authority. MSVC caught it as a non covariant return type and
	// then cascaded into six unrelated looking errors about bind_method and memdelete.
	virtual uint32_t _get_audio_mix_rate() const override;
	virtual AudioServer::SpeakerMode _get_audio_speaker_mode() const override;
	virtual Error _write_begin(const Vector2i &p_movie_size, uint32_t p_fps,
			const String &p_base_path) override;
	virtual Error _write_frame(const Ref<Image> &p_image, const void *p_audio_data) override;
	virtual void _write_end() override;

	void set_quality(int p_quality);
	int get_quality() const;
	void set_thread_count(int p_threads);
	int get_thread_count() const;
	void set_keep_alpha(bool p_keep);
	bool get_keep_alpha() const;
};

} // namespace godot

#endif // MOVIE_WRITER_CINEFORM_H
