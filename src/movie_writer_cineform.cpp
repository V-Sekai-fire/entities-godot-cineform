// SPDX-License-Identifier: Apache-2.0 OR MIT
#include "movie_writer_cineform.h"

#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <tmmintrin.h>   // SSSE3, for _mm_shuffle_epi8

#include <chrono>
#include <cstring>

using namespace godot;

// The encoder quality ladder, in the order Common/CFHDTypes.h declares it. Index 0 is the
// smallest file and index 5 is the largest. The names were read from the header, not recalled.
//
// CFHD_ENCODING_QUALITY_FIXED is deliberately absent. The header notes it is also read as
// unset, because the codec has no constant bitrate mode, so exposing it as a quality would
// offer a setting that silently means "no setting".
static const CFHD_EncodingQuality QUALITY_LADDER[] = {
	CFHD_ENCODING_QUALITY_LOW,
	CFHD_ENCODING_QUALITY_MEDIUM,
	CFHD_ENCODING_QUALITY_HIGH,
	CFHD_ENCODING_QUALITY_FILMSCAN1,
	CFHD_ENCODING_QUALITY_FILMSCAN2,
	CFHD_ENCODING_QUALITY_FILMSCAN3,
};
static const int QUALITY_COUNT = int(sizeof(QUALITY_LADDER) / sizeof(QUALITY_LADDER[0]));

static void put_u32(std::FILE *f, uint32_t v) {
	uint8_t b[4] = { uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24) };
	std::fwrite(b, 1, 4, f);
}

static void put_u16(std::FILE *f, uint16_t v) {
	uint8_t b[2] = { uint8_t(v), uint8_t(v >> 8) };
	std::fwrite(b, 1, 2, f);
}

static void put_tag(std::FILE *f, const char *t) {
	std::fwrite(t, 1, 4, f);
}

MovieWriterCineForm::MovieWriterCineForm() {
	quality_index = 2; // HIGH
	// 0 means "choose for me" HERE, and it is resolved in _write_begin. It does NOT mean that
	// to the SDK.
	//
	// RETRACTED: this used to pass 0 straight to CFHD_CreateEncoderPool, on the assumption the
	// pool would pick a thread count. It builds a pool with no encoders instead, and then
	// every frame fails in CEncoderPool::EncodeSample at `m_encoderList.size() == 0` with
	// CFHD_ERROR_UNEXPECTED, which is error code 10 and names nothing.
	//
	// Movie Maker reported this as a completed recording of 45 frames and wrote a 232 byte
	// file holding an AVI header and no video. Every layer above said success.
	thread_count = 0;
}

MovieWriterCineForm::~MovieWriterCineForm() {
	if (pool) {
		CFHD_StopEncoderPool(pool);
		CFHD_ReleaseEncoderPool(pool);
		pool = nullptr;
	}
	if (metadata) {
		CFHD_MetadataClose(metadata);
		metadata = nullptr;
	}
	if (avi) {
		std::fclose(avi);
		avi = nullptr;
	}
}

void MovieWriterCineForm::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_quality", "quality"), &MovieWriterCineForm::set_quality);
	ClassDB::bind_method(D_METHOD("get_quality"), &MovieWriterCineForm::get_quality);
	ClassDB::bind_method(D_METHOD("set_thread_count", "threads"),
			&MovieWriterCineForm::set_thread_count);
	ClassDB::bind_method(D_METHOD("get_thread_count"), &MovieWriterCineForm::get_thread_count);
	ClassDB::bind_method(D_METHOD("set_keep_alpha", "keep"),
			&MovieWriterCineForm::set_keep_alpha);
	ClassDB::bind_method(D_METHOD("get_keep_alpha"), &MovieWriterCineForm::get_keep_alpha);
	ClassDB::bind_method(D_METHOD("set_zero_copy", "zero_copy"),
			&MovieWriterCineForm::set_zero_copy);
	ClassDB::bind_method(D_METHOD("get_zero_copy"), &MovieWriterCineForm::get_zero_copy);
	ClassDB::bind_method(D_METHOD("set_flip_in_codec", "in_codec"),
			&MovieWriterCineForm::set_flip_in_codec);
	ClassDB::bind_method(D_METHOD("get_flip_in_codec"), &MovieWriterCineForm::get_flip_in_codec);
	ClassDB::bind_method(D_METHOD("set_hdr", "hdr"), &MovieWriterCineForm::set_hdr);
	ClassDB::bind_method(D_METHOD("get_hdr"), &MovieWriterCineForm::get_hdr);
}

void MovieWriterCineForm::set_quality(int p_quality) {
	quality_index = p_quality < 0 ? 0 : (p_quality >= QUALITY_COUNT ? QUALITY_COUNT - 1 : p_quality);
}
int MovieWriterCineForm::get_quality() const { return quality_index; }
void MovieWriterCineForm::set_thread_count(int p_threads) { thread_count = p_threads; }
int MovieWriterCineForm::get_thread_count() const { return thread_count; }
void MovieWriterCineForm::set_zero_copy(bool p_zero_copy) { zero_copy = p_zero_copy; }
bool MovieWriterCineForm::get_zero_copy() const { return zero_copy; }
void MovieWriterCineForm::set_flip_in_codec(bool p_in_codec) { flip_in_codec = p_in_codec; }
bool MovieWriterCineForm::get_flip_in_codec() const { return flip_in_codec; }
void MovieWriterCineForm::set_hdr(bool p_hdr) { hdr = p_hdr; }
bool MovieWriterCineForm::get_hdr() const { return hdr; }
void MovieWriterCineForm::set_keep_alpha(bool p_keep) { keep_alpha = p_keep; }
bool MovieWriterCineForm::get_keep_alpha() const { return keep_alpha; }

bool MovieWriterCineForm::_handles_file(const String &p_path) const {
	return p_path.get_extension().to_lower() == "cfhd";
}

// RETRACTED: this returned 0 and the writer recorded no sound.
//
// Godot sizes its mix buffer as mix_rate * channels / fps and hands one block with every
// frame. It also REFUSES TO START if mix_rate is not divisible by fps, which is why this
// reads the project setting rather than inventing a number: a caller who sets 44100 and
// records at 30 fps gets a clear error from the engine instead of silence from us.
uint32_t MovieWriterCineForm::_get_audio_mix_rate() const {
	return mix_rate;
}

AudioServer::SpeakerMode MovieWriterCineForm::_get_audio_speaker_mode() const {
	return AudioServer::SPEAKER_MODE_STEREO;
}

Error MovieWriterCineForm::_write_begin(const Vector2i &p_movie_size, uint32_t p_fps,
		const String &p_base_path) {
	size = p_movie_size;
	frame_rate = p_fps;
	frame_index = 0;
	frames_written = 0;
	queued = 0;
	index.clear();

	// CineForm encodes in 8x8 wavelet blocks. An odd width or height is not representable,
	// and the SDK reports it late and unhelpfully, so it is checked here where the caller can
	// still read the message.
	if ((size.x & 1) || (size.y & 1)) {
		UtilityFunctions::printerr(
				"CineForm needs even width and height. Got ", size.x, "x", size.y,
				". Set the movie size to even numbers in Project Settings.");
		return ERR_INVALID_PARAMETER;
	}

	// OS::get_processor_count rather than std::thread::hardware_concurrency. The standard
	// library call is permitted to return 0 and does so on some platforms, which would land
	// straight back in the zero encoder bug below. Godot answers the same question for every
	// platform it supports, and gen_bindings.py gates the method across 4.0 to 4.5.
	int threads = thread_count;
	if (threads <= 0) {
		threads = OS::get_singleton()->get_processor_count();
		if (threads < 1) {
			threads = 4;
		}
		if (threads > 16) {
			threads = 16; // the job queue is 8 deep, so more encoders than that just idle
		}
	}
	// Ask the ENGINE what it renders, and pick the encoder format from that. An HDR viewport
	// hands over half or full float, and packing that into 8 bits discards it before the
	// codec is involved.
	// TWO PROFILES, from the formats CineForm documents as supported.
	//
	//   SDR   BGRA 8-bit in  -> RGB 4:4:4   compressed at 12-bit, gamma 2.2 (the default)
	//   HDR   RG64 16-bit in -> RGBA 4:4:4:4 compressed at 12-bit, log encoded
	//
	// CineForm has no HDR transfer function. There is no PQ, no HLG and no BT.2020 anywhere
	// in the SDK, and TAG_COLORSPACE_YUV knows only 601 and 709. Its answer to dynamic range
	// is the film scan one: log encode into 12 bits and record which curve was used, so a
	// decoder can invert it. That is latitude, not HDR10, and the distinction belongs in the
	// log line below rather than in a reader's assumptions.
	//
	// The choice is made HERE because _write_begin runs before any frame arrives, so the
	// image format cannot be consulted. The viewport setting is what decides what Godot will
	// hand over.
	// Project settings rather than environment variables, so the choice travels with the
	// project and shows up in the editor.
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!hdr) {
		hdr = bool(ps->get_setting_with_override("cineform/hdr")) ||
				bool(ps->get_setting_with_override("rendering/viewport/hdr_2d"));
	}
	flip_in_codec = bool(ps->get_setting_with_override("cineform/flip_in_codec"));
	zero_copy = bool(ps->get_setting_with_override("cineform/zero_copy"));
	{
		const Variant q = ps->get_setting_with_override("cineform/quality");
		if (q.get_type() == Variant::INT) {
			set_quality(int(q));
		}
	}
	if (zero_copy) {
		// Nothing is converted, so the codec must do the row order itself.
		flip_in_codec = true;
	}
	if (hdr) {
		pixel_format = CFHD_PIXEL_FORMAT_RG64; // 16-bit RGBA, the encoder's first preference
		bytes_per_pixel = 8;
	} else {
		// BGRa asks the codec to reverse the rows. BGRA means we do it.
		pixel_format = flip_in_codec ? CFHD_PIXEL_FORMAT_BGRa : CFHD_PIXEL_FORMAT_BGRA;
		bytes_per_pixel = 4;
	}

	mix_rate = uint32_t(int64_t(ProjectSettings::get_singleton()->get_setting_with_override(
			"editor/movie_writer/mix_rate")));
	if (mix_rate == 0) {
		mix_rate = 48000;
	}
	audio_channels = 2; // matches _get_audio_speaker_mode returning SPEAKER_MODE_STEREO

	CFHD_Error err = CFHD_CreateEncoderPool(&pool, threads, 8, nullptr);
	if (err != CFHD_ERROR_OKAY) {
		UtilityFunctions::printerr("CFHD_CreateEncoderPool failed, code ", int(err));
		return ERR_CANT_CREATE;
	}

	// BGRA in, because that is what Godot's RGBA8 becomes after the swap in _write_frame.
	// RGBA_4444 keeps alpha, RGB_444 drops it. Movie Maker output is usually opaque, so
	// alpha is off by default and costs nothing when it is not wanted.
	const CFHD_EncodedFormat encoded =
			(keep_alpha || hdr) ? CFHD_ENCODED_FORMAT_RGBA_4444 : CFHD_ENCODED_FORMAT_RGB_444;
	// LOG90 is what the SDK recommends for wide latitude sources. Under it the encoder writes
	// TAG_ENCODE_CURVE, so the curve travels with the file and a decoder can undo it.
	const CFHD_EncodingFlags flags =
			hdr ? CFHD_ENCODING_FLAGS_CURVE_LOG90 : CFHD_ENCODING_FLAGS_NONE;
	err = CFHD_PrepareEncoderPool(pool, uint_least16_t(size.x), uint_least16_t(size.y),
			pixel_format, encoded, flags, QUALITY_LADDER[quality_index]);
	if (err != CFHD_ERROR_OKAY) {
		UtilityFunctions::printerr("CFHD_PrepareEncoderPool failed, code ", int(err));
		return ERR_CANT_CREATE;
	}

	// A format the encoder does not accept fails per frame, deep inside, with
	// CFHD_ERROR_UNEXPECTED and no name attached. Asking first turns that into one message
	// here. This is how W13A was ruled out: it is in the pixel format enum and it is not in
	// this list.
	{
		CFHD_PixelFormat accepted[64];
		int count = 0;
		if (CFHD_GetAsyncInputFormats(pool, accepted, 64, &count) == CFHD_ERROR_OKAY) {
			bool ok = false;
			for (int i = 0; i < count; i++) {
				if (accepted[i] == pixel_format) {
					ok = true;
					break;
				}
			}
			if (!ok) {
				UtilityFunctions::printerr(
						"CineForm: the encoder does not accept the chosen pixel format. "
						"It advertises ", count, " formats and this is not one of them.");
				return ERR_INVALID_PARAMETER;
			}
		}
	}

	err = CFHD_StartEncoderPool(pool);
	if (err != CFHD_ERROR_OKAY) {
		UtilityFunctions::printerr("CFHD_StartEncoderPool failed, code ", int(err));
		return ERR_CANT_CREATE;
	}
	UtilityFunctions::print("CineForm: ", size.x, "x", size.y, " at ", frame_rate,
			" fps, quality ", quality_index, ", ", threads, " encoder threads, ",
			zero_copy && !hdr
				? "SDR profile (zero copy, Godot buffer straight to the codec)"
			: hdr ? "HDR profile (RG64 16-bit in, log encoded, 12-bit RGBA 4:4:4:4)"
				: (flip_in_codec
					? "SDR profile (BGRa 8-bit in, codec flips rows, 12-bit RGB 4:4:4)"
					: "SDR profile (BGRA 8-bit in, we flip rows, 12-bit RGB 4:4:4)"),
			", audio ", mix_rate, " Hz x", audio_channels);

	begin_avi(p_base_path.get_basename() + ".cfhd");
	if (!avi) {
		return ERR_FILE_CANT_WRITE;
	}
	staging.resize(size_t(size.x) * size_t(size.y) * size_t(bytes_per_pixel));
	audio_staging.resize(size_t(mix_rate / (frame_rate ? frame_rate : 30)) * size_t(audio_channels));
	return OK;
}

Error MovieWriterCineForm::_write_frame(const Ref<Image> &p_image, const void *p_audio_data) {
	if (!pool || !avi || p_image.is_null()) {
		return ERR_UNCONFIGURED;
	}

	const auto t_convert = std::chrono::steady_clock::now();
	Ref<Image> img = p_image;
	// The HDR profile needs the float data preserved, so it converts to RGBAF rather than
	// flattening to 8 bits first. Converting p_image in place would mutate a frame the engine
	// still owns, and `Image` has no duplicate of its own: it inherits Resource::duplicate,
	// which returns Ref<Resource> and does not assign back to a Ref<Image>.
	const Image::Format want = hdr ? Image::FORMAT_RGBAF : Image::FORMAT_RGBA8;
	if (img->get_format() != want) {
		Ref<Image> copy = Image::create_from_data(img->get_width(), img->get_height(), false,
				img->get_format(), img->get_data());
		copy->convert(want);
		img = copy;
	}

	const PackedByteArray src = img->get_data();
	const int w = size.x, h = size.y;
	const size_t src_pitch = size_t(w) * (hdr ? 16u : 4u);
	const size_t pitch = size_t(w) * size_t(bytes_per_pixel);
	if (size_t(src.size()) < src_pitch * size_t(h)) {
		UtilityFunctions::printerr("frame is ", src.size(), " bytes, expected ",
				src_pitch * h);
		return ERR_INVALID_DATA;
	}

	// Rows are flipped in both paths. AVI stores bottom up, and the pool API does not
	// document a negative pitch, so the flip happens here rather than by passing one.
	const uint8_t *in = src.ptr();
	if (zero_copy && !hdr) {
		// Nothing to do. The bytes go to the encoder as Godot produced them.
	} else if (hdr) {
		// RG64 is 16-bit RGBA. Godot's floats are scene referred and may exceed 1.0, which is
		// the whole point of recording HDR, so they are scaled and CLAMPED rather than
		// wrapped. A wrap turns a highlight into a black hole and looks like a codec bug.
		for (int y = 0; y < h; y++) {
			const float *s16 = reinterpret_cast<const float *>(in + src_pitch * size_t(h - 1 - y));
			uint16_t *d = reinterpret_cast<uint16_t *>(staging.data() + pitch * size_t(y));
			for (int x = 0; x < w; x++) {
				for (int c = 0; c < 4; c++) {
					float v = s16[c] * 65535.0f;
					if (v < 0.0f) {
						v = 0.0f;
					} else if (v > 65535.0f) {
						v = 65535.0f;
					}
					d[c] = uint16_t(v);
				}
				s16 += 4;
				d += 4;
			}
		}
	} else {
		// RGBA to BGRA, sixteen bytes at a time.
		//
		// MEASURED, and this is why it is not the obvious byte loop. At 3840 by 2160 the
		// scalar version cost 25.2 ms for each frame while the codec itself waited 10.2 ms.
		// The conversion was two and a half times the encoder. 8.3 million pixels moved one
		// byte at a time is 66 MB of traffic per frame with no instruction level parallelism.
		//
		// SSSE3 is safe to require here. cineform-sdk already includes <emmintrin.h>
		// unconditionally, so this build is x86 only regardless, and every x86_64 part since
		// Core 2 in 2006 has pshufb.
		//
		// The row flip stays free either way. It is pointer arithmetic, not data movement.
		const __m128i swizzle = _mm_setr_epi8(2, 1, 0, 3, 6, 5, 4, 7,
				10, 9, 8, 11, 14, 13, 12, 15);
		const int wide = (w * 4) & ~15;
		for (int y = 0; y < h; y++) {
			const uint8_t *s8 = in + src_pitch * size_t(flip_in_codec ? y : (h - 1 - y));
			uint8_t *d = staging.data() + pitch * size_t(y);
			int b = 0;
			for (; b < wide; b += 16) {
				const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i *>(s8 + b));
				_mm_storeu_si128(reinterpret_cast<__m128i *>(d + b),
						_mm_shuffle_epi8(v, swizzle));
			}
			// The tail, for a width that is not a multiple of four pixels.
			for (; b < w * 4; b += 4) {
				d[b + 0] = s8[b + 2];
				d[b + 1] = s8[b + 1];
				d[b + 2] = s8[b + 0];
				d[b + 3] = s8[b + 3];
			}
		}
	}

	const auto t_submit = std::chrono::steady_clock::now();
	convert_us += std::chrono::duration<double, std::micro>(t_submit - t_convert).count();

	// Zero copy hands the encoder Godot's own bytes. `src` is a COW handle onto the image
	// data, so keeping it in `inflight` keeps the memory alive until the sample comes back.
	const void *buffer = staging.data();
	intptr_t buffer_pitch = intptr_t(pitch);
	if (zero_copy && !hdr) {
		inflight[frame_index] = src;
		buffer = inflight[frame_index].ptr();
		buffer_pitch = intptr_t(src_pitch);
	}

	CFHD_Error err = CFHD_EncodeAsyncSample(pool, frame_index, const_cast<void *>(buffer),
			buffer_pitch, metadata);
	if (err != CFHD_ERROR_OKAY) {
		UtilityFunctions::printerr("CFHD_EncodeAsyncSample failed on frame ", frame_index,
				", code ", int(err));
		return ERR_CANT_CREATE;
	}
	frame_index++;
	queued++;

	// Audio rides alongside. Godot sizes the block as mix_rate * channels / fps and hands one
	// with every frame, so a dropped block is a gap in the sound, not a shorter file.
	if (p_audio_data != nullptr) {
		write_audio(static_cast<const int32_t *>(p_audio_data));
	}

	const auto t_drain = std::chrono::steady_clock::now();
	submit_us += std::chrono::duration<double, std::micro>(t_drain - t_submit).count();

	// Collect whatever is finished without waiting. The pool runs ahead of us, so blocking on
	// every frame would give up the threading we asked for.
	drain(false);
	drain_us += std::chrono::duration<double, std::micro>(
			std::chrono::steady_clock::now() - t_drain).count();
	return OK;
}

void MovieWriterCineForm::drain(bool block) {
	while (queued > 0) {
		uint32_t number = 0;
		CFHD_SampleBufferRef buffer = nullptr;
		CFHD_Error err = block ? CFHD_WaitForSample(pool, &number, &buffer)
							   : CFHD_TestForSample(pool, &number, &buffer);
		if (err != CFHD_ERROR_OKAY || buffer == nullptr) {
			break;
		}
		void *data = nullptr;
		size_t len = 0;
		if (CFHD_GetEncodedSample(buffer, &data, &len) == CFHD_ERROR_OKAY && data && len) {
			write_sample(data, len);
		}
		CFHD_ReleaseSampleBuffer(pool, buffer);
		// The encoder is done with this frame, so the data it read from can go.
		inflight.erase(number);
		queued--;
	}
}

void MovieWriterCineForm::_write_end() {
	if (pool) {
		drain(true); // everything still in flight, or the tail of the movie is lost
		CFHD_StopEncoderPool(pool);
		CFHD_ReleaseEncoderPool(pool);
		pool = nullptr;
	}
	finish_avi();
	inflight.clear();
	staging.clear();
	staging.shrink_to_fit();
	UtilityFunctions::print("CineForm: wrote ", frames_written, " frames of ", frame_index,
			" submitted");
	if (frame_index) {
		const double n = double(frame_index);
		UtilityFunctions::print("CineForm: per frame, convert ", convert_us / n / 1000.0,
				" ms, submit ", submit_us / n / 1000.0, " ms, drain ",
				drain_us / n / 1000.0, " ms");
	}
	if (frames_written != frame_index) {
		// Not cosmetic. A silent shortfall here is a truncated movie that still plays.
		UtilityFunctions::printerr("CineForm: ", frame_index - frames_written,
				" frames were submitted and never written");
	}
}

// --- the container -------------------------------------------------------------------
//
// AVI, because the SDK ships an AVI reader in Example/ and because CineForm in AVI is what
// GoPro's own tools emit. Matroska is the workspace's container of record, so the corpus
// path remuxes with `ffmpeg -c copy`, which is a stream copy and re-encodes nothing.
//
// The header is written with placeholder sizes and patched in finish_avi, because the totals
// are not known until the last frame has been encoded.

void MovieWriterCineForm::begin_avi(const String &p_path) {
	avi = std::fopen(p_path.utf8().get_data(), "wb");
	if (!avi) {
		UtilityFunctions::printerr("cannot open ", p_path, " for writing");
		return;
	}
	const uint32_t rate = frame_rate ? frame_rate : 30;

	put_tag(avi, "RIFF");
	riff_size_at = std::ftell(avi);
	put_u32(avi, 0);
	put_tag(avi, "AVI ");

	const uint32_t hdrl_bytes = 4 + 8 + 56                 // avih
			+ 8 + 4 + 8 + 56 + 8 + 40                       // video strl
			+ 8 + 4 + 8 + 56 + 8 + 18;                      // audio strl
	put_tag(avi, "LIST");
	put_u32(avi, hdrl_bytes);
	put_tag(avi, "hdrl");

	put_tag(avi, "avih");
	put_u32(avi, 56);
	put_u32(avi, 1000000u / rate); // microseconds per frame
	put_u32(avi, 0);
	put_u32(avi, 0);
	put_u32(avi, 0x00000110); // HASINDEX | ISINTERLEAVED
	frames_field_at = std::ftell(avi);
	put_u32(avi, 0); // total frames, patched later
	put_u32(avi, 0);
	put_u32(avi, 2); // dwStreams: video and audio
	put_u32(avi, 0);
	put_u32(avi, uint32_t(size.x));
	put_u32(avi, uint32_t(size.y));
	for (int i = 0; i < 4; i++) {
		put_u32(avi, 0);
	}

	put_tag(avi, "LIST");
	put_u32(avi, 4 + 8 + 56 + 8 + 40);
	put_tag(avi, "strl");

	put_tag(avi, "strh");
	put_u32(avi, 56);
	put_tag(avi, "vids");
	put_tag(avi, "CFHD");
	put_u32(avi, 0);
	put_u32(avi, 0);
	put_u32(avi, 0);
	put_u32(avi, 1);    // scale
	put_u32(avi, rate); // rate, so rate/scale is frames each second
	put_u32(avi, 0);
	// dwLength, the stream length in frames. Written as a placeholder and patched in
	// finish_avi. It said "patched later" and was not, so it stayed 0, and every reader that
	// derives duration from the STREAM header rather than the main header reported no
	// duration at all. ffprobe printed `duration=N/A` on a thirty second file.
	stream_len_at = std::ftell(avi);
	put_u32(avi, 0);
	put_u32(avi, 0);
	put_u32(avi, 0xFFFFFFFFu);
	put_u32(avi, 0);
	put_u32(avi, 0);
	put_u32(avi, uint32_t(size.x) | (uint32_t(size.y) << 16));

	put_tag(avi, "strf");
	put_u32(avi, 40);
	put_u32(avi, 40);
	put_u32(avi, uint32_t(size.x));
	put_u32(avi, uint32_t(size.y));
	put_u32(avi, 1 | (24u << 16));
	put_tag(avi, "CFHD");
	put_u32(avi, uint32_t(size.x) * uint32_t(size.y) * 3);
	for (int i = 0; i < 4; i++) {
		put_u32(avi, 0);
	}

	// The audio stream. WAVEFORMATEX is 18 bytes with cbSize, and writing the 16 byte
	// PCMWAVEFORMAT instead is the usual way to produce an AVI that some players open and
	// others reject.
	put_tag(avi, "LIST");
	put_u32(avi, 4 + 8 + 56 + 8 + 18);
	put_tag(avi, "strl");

	const uint32_t block_align = uint32_t(audio_channels) * 2u;
	put_tag(avi, "strh");
	put_u32(avi, 56);
	put_tag(avi, "auds");
	put_u32(avi, 1); // WAVE_FORMAT_PCM as the handler
	put_u32(avi, 0);
	put_u32(avi, 0);
	put_u32(avi, 0);
	put_u32(avi, block_align);            // dwScale, one sample frame
	put_u32(avi, mix_rate);               // dwRate
	put_u32(avi, 0);
	audio_len_at = std::ftell(avi);
	put_u32(avi, 0);                      // dwLength in sample frames, patched later
	put_u32(avi, mix_rate * block_align); // dwSuggestedBufferSize, one second
	put_u32(avi, 0xFFFFFFFFu);
	put_u32(avi, block_align);            // dwSampleSize
	put_u32(avi, 0);
	put_u32(avi, 0);

	put_tag(avi, "strf");
	put_u32(avi, 18);
	put_u16(avi, 1);                              // wFormatTag, PCM
	put_u16(avi, uint16_t(audio_channels));
	put_u32(avi, mix_rate);
	put_u32(avi, mix_rate * block_align);         // nAvgBytesPerSec
	put_u16(avi, uint16_t(block_align));
	put_u16(avi, 16);                             // wBitsPerSample
	put_u16(avi, 0);                              // cbSize

	put_tag(avi, "LIST");
	movi_size_at = std::ftell(avi);
	put_u32(avi, 0);
	put_tag(avi, "movi");
	movi_start = std::ftell(avi);
}

void MovieWriterCineForm::write_sample(const void *p_data, size_t p_len) {
	if (!avi) {
		return;
	}
	const long here = std::ftell(avi);
	put_tag(avi, "00dc");
	put_u32(avi, uint32_t(p_len));
	std::fwrite(p_data, 1, p_len, avi);
	if (p_len & 1) {
		const uint8_t pad = 0;
		std::fwrite(&pad, 1, 1, avi); // RIFF chunks are word aligned
	}
	Entry e;
	e.offset = uint32_t(here - movi_start + 4);
	e.size = uint32_t(p_len);
	e.stream = 0;
	index.push_back(e);
	frames_written++;
}

void MovieWriterCineForm::write_audio(const int32_t *p_samples) {
	if (!avi || audio_staging.empty()) {
		return;
	}
	// Godot mixes into int32. AVI PCM here is 16-bit, which is what every player reads
	// without negotiation, so the top 16 bits are kept and the rest discarded.
	const size_t n = audio_staging.size();
	for (size_t i = 0; i < n; i++) {
		audio_staging[i] = int16_t(p_samples[i] >> 16);
	}
	const size_t bytes = n * sizeof(int16_t);
	const long here = std::ftell(avi);
	put_tag(avi, "01wb");
	put_u32(avi, uint32_t(bytes));
	std::fwrite(audio_staging.data(), 1, bytes, avi);
	if (bytes & 1) {
		const uint8_t pad = 0;
		std::fwrite(&pad, 1, 1, avi);
	}
	Entry e;
	e.offset = uint32_t(here - movi_start + 4);
	e.size = uint32_t(bytes);
	e.stream = 1;
	index.push_back(e);
	audio_bytes += uint32_t(bytes);
}


void MovieWriterCineForm::finish_avi() {
	if (!avi) {
		return;
	}
	const long movi_end = std::ftell(avi);

	put_tag(avi, "idx1");
	put_u32(avi, uint32_t(index.size() * 16));
	for (size_t i = 0; i < index.size(); i++) {
		// The index entry has to name the stream it belongs to. Tagging every entry 00dc
		// would tell a player the audio chunks are video, and it would try to decode them.
		put_tag(avi, index[i].stream == 0 ? "00dc" : "01wb");
		put_u32(avi, 0x10); // AVIIF_KEYFRAME. Every CineForm frame is a keyframe.
		put_u32(avi, index[i].offset);
		put_u32(avi, index[i].size);
	}
	const long end = std::ftell(avi);

	std::fseek(avi, riff_size_at, SEEK_SET);
	put_u32(avi, uint32_t(end - riff_size_at - 4));
	std::fseek(avi, movi_size_at, SEEK_SET);
	put_u32(avi, uint32_t(movi_end - movi_size_at - 4));
	std::fseek(avi, frames_field_at, SEEK_SET);
	put_u32(avi, frames_written);
	std::fseek(avi, stream_len_at, SEEK_SET);
	put_u32(avi, frames_written);
	// The audio stream length is in SAMPLES, not bytes and not chunks. dwSampleSize for this
	// stream is the frame size in bytes, so dwLength must be the count of those units.
	std::fseek(avi, audio_len_at, SEEK_SET);
	put_u32(avi, audio_bytes / uint32_t(audio_channels * 2));

	std::fclose(avi);
	avi = nullptr;
}
