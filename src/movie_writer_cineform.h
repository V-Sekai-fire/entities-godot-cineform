// SPDX-License-Identifier: Apache-2.0 OR MIT
#ifndef MOVIE_WRITER_CINEFORM_H
#define MOVIE_WRITER_CINEFORM_H

#include <godot_cpp/classes/movie_writer.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <cstdio>
#include <map>
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

	// The pixel format handed to the encoder, chosen in _write_begin from what Godot renders.
	//
	// RG64 is 16-bit RGBA and is the encoder's FIRST preference in
	// CSampleEncoder::GetInputFormats. BGRA is 8-bit and sits eleventh. When the viewport is
	// HDR, Godot hands us half or full float, and 8-bit would throw that away before the
	// codec ever sees it.
	//
	// W13A and WP13 look like the right answer for HDR, being signed 16-bit with the
	// whitepoint at 1<<13, and they are NOT usable. GetInputFormats does not advertise
	// either. They appear only in VideoBuffers.cpp pitch arithmetic.
	CFHD_PixelFormat pixel_format = CFHD_PIXEL_FORMAT_BGRA;
	int bytes_per_pixel = 4;
	bool hdr = false;

	// Who flips the rows, us or the codec.
	//
	// AVI stores bottom up. CFHD_PIXEL_FORMAT_BGRA maps to COLOR_FORMAT_BGRA and expects that
	// order, so our loop reverses the rows. CFHD_PIXEL_FORMAT_BGRa maps to
	// COLOR_FORMAT_RGB32_INVERTED, and the codec does the reversal itself.
	//
	// Both are in the encoder's accepted list. Which is faster is a measurement, not a guess,
	// so both are reachable and `flip_in_codec` selects between them.
	bool flip_in_codec = false;

	// Hand the encoder Godot's own buffer instead of a converted copy.
	//
	// THE LIFETIME IS THE WHOLE PROBLEM. CFHD_EncodeAsyncSample queues an EncoderJob holding
	// the pointer and returns immediately. A worker thread reads it later, after _write_frame
	// has returned and after Godot has moved on. A pointer into a buffer released at the end
	// of the call is read after free, and the symptom is a frame of garbage somewhere in the
	// middle of a recording rather than a crash.
	//
	// So each frame's data is kept in `inflight`, keyed by frame number, and released when
	// the matching sample comes back out of the pool.
	bool zero_copy = false;
	std::map<uint32_t, PackedByteArray> inflight;

	// Godot hands us RGBA8 top down. CineForm's BGRA input wants the rows the other way and
	// the channels swapped, so this is where that happens. Doing it in place on Godot's
	// buffer would mutate a frame the engine still owns.
	std::vector<uint8_t> staging;

	// Audio. Godot hands one block of int32 samples with every frame, sized
	// mix_rate * channels / fps, and it refuses to start if mix_rate is not divisible by fps.
	uint32_t mix_rate = 48000;
	int audio_channels = 2;
	uint32_t audio_bytes = 0;
	long audio_len_at = 0;
	long audio_bytes_at = 0;
	std::vector<int16_t> audio_staging;

	// Where the time actually goes. Godot reports one "Encoding time" covering all of
	// _write_frame, so a slow conversion and a slow codec are indistinguishable from it.
	double convert_us = 0.0;
	double submit_us = 0.0;
	double drain_us = 0.0;

	// AVI index. Every sample offset and size, so the idx1 chunk can be written at the end.
	// `stream` is 0 for video and 1 for audio, because idx1 tags each entry with its stream
	// and a single stream assumption here writes an index no player can follow.
	struct Entry {
		uint32_t offset;
		uint32_t size;
		int stream;
	};
	std::vector<Entry> index;
	long movi_start = 0;
	long riff_size_at = 0;
	long movi_size_at = 0;
	long frames_field_at = 0;
	long stream_len_at = 0;

	void drain(bool block);
	void write_sample(const void *data, size_t len);
	void write_audio(const int32_t *samples);
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
	void set_zero_copy(bool p_zero_copy);
	bool get_zero_copy() const;
	void set_flip_in_codec(bool p_in_codec);
	bool get_flip_in_codec() const;
	void set_hdr(bool p_hdr);
	bool get_hdr() const;
	void set_keep_alpha(bool p_keep);
	bool get_keep_alpha() const;
};

} // namespace godot

#endif // MOVIE_WRITER_CINEFORM_H
