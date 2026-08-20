// SPDX-License-Identifier: Apache-2.0 OR MIT
#include "movie_writer_cineform.h"

#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

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
}

void MovieWriterCineForm::set_quality(int p_quality) {
	quality_index = p_quality < 0 ? 0 : (p_quality >= QUALITY_COUNT ? QUALITY_COUNT - 1 : p_quality);
}
int MovieWriterCineForm::get_quality() const { return quality_index; }
void MovieWriterCineForm::set_thread_count(int p_threads) { thread_count = p_threads; }
int MovieWriterCineForm::get_thread_count() const { return thread_count; }
void MovieWriterCineForm::set_keep_alpha(bool p_keep) { keep_alpha = p_keep; }
bool MovieWriterCineForm::get_keep_alpha() const { return keep_alpha; }

bool MovieWriterCineForm::_handles_file(const String &p_path) const {
	return p_path.get_extension().to_lower() == "cfhd";
}

// No audio. Returning a real mix rate would make Godot allocate and hand us audio blocks we
// then drop on the floor, which reads at the call site as if sound were being recorded.
uint32_t MovieWriterCineForm::_get_audio_mix_rate() const {
	return 0;
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
	CFHD_Error err = CFHD_CreateEncoderPool(&pool, threads, 8, nullptr);
	if (err != CFHD_ERROR_OKAY) {
		UtilityFunctions::printerr("CFHD_CreateEncoderPool failed, code ", int(err));
		return ERR_CANT_CREATE;
	}

	// BGRA in, because that is what Godot's RGBA8 becomes after the swap in _write_frame.
	// RGBA_4444 keeps alpha, RGB_444 drops it. Movie Maker output is usually opaque, so
	// alpha is off by default and costs nothing when it is not wanted.
	const CFHD_EncodedFormat encoded =
			keep_alpha ? CFHD_ENCODED_FORMAT_RGBA_4444 : CFHD_ENCODED_FORMAT_RGB_444;
	err = CFHD_PrepareEncoderPool(pool, uint_least16_t(size.x), uint_least16_t(size.y),
			CFHD_PIXEL_FORMAT_BGRA, encoded, CFHD_ENCODING_FLAGS_NONE,
			QUALITY_LADDER[quality_index]);
	if (err != CFHD_ERROR_OKAY) {
		UtilityFunctions::printerr("CFHD_PrepareEncoderPool failed, code ", int(err));
		return ERR_CANT_CREATE;
	}

	err = CFHD_StartEncoderPool(pool);
	if (err != CFHD_ERROR_OKAY) {
		UtilityFunctions::printerr("CFHD_StartEncoderPool failed, code ", int(err));
		return ERR_CANT_CREATE;
	}
	UtilityFunctions::print("CineForm: ", size.x, "x", size.y, " at ", frame_rate,
			" fps, quality ", quality_index, ", ", threads, " encoder threads");

	begin_avi(p_base_path.get_basename() + ".cfhd");
	if (!avi) {
		return ERR_FILE_CANT_WRITE;
	}
	staging.resize(size_t(size.x) * size_t(size.y) * 4);
	return OK;
}

Error MovieWriterCineForm::_write_frame(const Ref<Image> &p_image, const void *p_audio_data) {
	if (!pool || !avi || p_image.is_null()) {
		return ERR_UNCONFIGURED;
	}

	Ref<Image> img = p_image;
	if (img->get_format() != Image::FORMAT_RGBA8) {
		// Convert on a COPY. Calling convert on p_image would mutate a frame the engine still
		// owns, and that corruption would surface somewhere unrelated.
		//
		// `create_from_data` rather than `duplicate`. Image has no duplicate of its own. It
		// inherits Resource::duplicate, which returns Ref<Resource>, so assigning it back to a
		// Ref<Image> does not compile. `create_from_data` is static, returns Image, and is
		// present unchanged in the 4.1 and 4.5 APIs.
		Ref<Image> copy = Image::create_from_data(img->get_width(), img->get_height(), false,
				img->get_format(), img->get_data());
		copy->convert(Image::FORMAT_RGBA8);
		img = copy;
	}

	const PackedByteArray src = img->get_data();
	const int w = size.x, h = size.y;
	const size_t pitch = size_t(w) * 4;
	if (size_t(src.size()) < pitch * size_t(h)) {
		UtilityFunctions::printerr("frame is ", src.size(), " bytes, expected ", pitch * h);
		return ERR_INVALID_DATA;
	}

	// RGBA top down to BGRA bottom up, in one pass. AVI stores rows bottom up, and the SDK
	// takes the pitch as given, so the flip happens here rather than by passing a negative
	// pitch, which the pool API does not document.
	const uint8_t *in = src.ptr();
	for (int y = 0; y < h; y++) {
		const uint8_t *s = in + pitch * size_t(h - 1 - y);
		uint8_t *d = staging.data() + pitch * size_t(y);
		for (int x = 0; x < w; x++) {
			d[0] = s[2];
			d[1] = s[1];
			d[2] = s[0];
			d[3] = s[3];
			s += 4;
			d += 4;
		}
	}

	CFHD_Error err = CFHD_EncodeAsyncSample(pool, frame_index, staging.data(),
			intptr_t(pitch), metadata);
	if (err != CFHD_ERROR_OKAY) {
		UtilityFunctions::printerr("CFHD_EncodeAsyncSample failed on frame ", frame_index,
				", code ", int(err));
		return ERR_CANT_CREATE;
	}
	frame_index++;
	queued++;

	// Collect whatever is finished without waiting. The pool runs ahead of us, so blocking on
	// every frame would give up the threading we asked for.
	drain(false);
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
	staging.clear();
	staging.shrink_to_fit();
	UtilityFunctions::print("CineForm: wrote ", frames_written, " frames of ", frame_index,
			" submitted");
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

	put_tag(avi, "LIST");
	put_u32(avi, 4 + 8 + 56 + 8 + 4 + 8 + 56 + 8 + 40);
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
	put_u32(avi, 1);
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
	index.push_back(e);
	frames_written++;
}

void MovieWriterCineForm::finish_avi() {
	if (!avi) {
		return;
	}
	const long movi_end = std::ftell(avi);

	put_tag(avi, "idx1");
	put_u32(avi, uint32_t(index.size() * 16));
	for (size_t i = 0; i < index.size(); i++) {
		put_tag(avi, "00dc");
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

	std::fclose(avi);
	avi = nullptr;
}
