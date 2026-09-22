/* SPDX-FileCopyrightText: 2024-2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup imbuf
 *
 * Movie file reading / playback functions.
 */

#pragma once

#include "IMB_imbuf_enums.h"

#include "BLI_function_ref.hh"
#include "BLI_set.hh"

#include <string>

namespace blender {

struct IDProperty;
struct ImBuf;
struct MovieReader;
struct MovieProxyBuilder;

/**
 * Opens a movie file for reading / playback.
 * From ib_flags only ImBufFlags::Deinterlace is taken into account.
 * streamindex is for multi-track movie files.
 *
 * Returned MovieReader object can be used in other playback related functions.
 * Note that a valid object will be returned even if file does not exist or is
 * not a video file. The actual initialization is delayed until
 * #MOV_decode_frame is called.
 *
 * When done with playback, use #MOV_close to delete it.
 */
MovieReader *MOV_open_file(const char *filepath,
                           ImBufFlags ib_flags,
                           int streamindex,
                           bool keep_original_colorspace,
                           char colorspace[IM_MAX_SPACE]);

/**
 * Release memory and other resources associated with movie playback.
 */
void MOV_close(MovieReader *anim);

/**
 * Fetches a frame from a movie at given frame position.
 *
 * Internally this will seek within the movie as/if needed. For most movie
 * files, decoding frames sequentially is much more efficient than decoding
 * random frames.
 *
 * If proxy_size is not IMB_PROXY_NONE, a proxy file of given size will
 * be attempted. If it exists, the frame will be decoded from it. If the
 * proxy does not exist, original file will be used.
 *
 * Movies that are <= 8 bits/color channel are returned as byte images;
 * higher bit depth movies are returned as float images. Note that the
 * color space is returned as-is, i.e. a float image might not be in
 * linear space.
 *
 * Returned image can be null if movie file does not exist, is not supported
 * or failed decoding.
 */
ImBuf *MOV_decode_frame(MovieReader *anim,
                        int position,
                        IMB_Proxy_Size preview_size /* = 0 = IMB_PROXY_NONE */);

/**
 * Falcon: 復号したままの YUV の面(RGB に変換しない)。GPU 再生の経路が、面をそのまま
 * テクスチャへ送ってシェーダで RGB にするために使う(CPU の色変換と RGBA の転送を省く)。
 */
struct MovieYUVFrame {
  void *frame = nullptr; /* AVFrame(参照を 1 つ持つ)。`MOV_yuv_frame_free` で手放す。 */
  int width = 0;
  int height = 0;
  /** 1 = NV12(8bit・Y + UV の 2 面)/ 2 = P010(10bit・2 面)/ 3 = YUV420P(8bit・3 面)/
   *  4 = NV12・5 = P010 の **GPU 上の面**(`MOV_yuv_plane_copy_to_gl_buffer` で写す・段 G2)。 */
  int layout = 0;
  bool full_range = false;
  /** YUV → RGB の係数: 601 / 709 / 2020(sws と同じ選び方)。 */
  int matrix = 601;
  /** RGB にした時の色空間の名前(byte の絵に付く物と同じ)。 */
  const char *colorspace = nullptr;
};

/** 求めるコマを復号して YUV の面のまま返す。回転・インターレース解除・対応外の形式では false。 */
bool MOV_decode_frame_yuv(MovieReader *anim, int position, MovieYUVFrame &r_frame);
const uint8_t *MOV_yuv_plane(const MovieYUVFrame &frame, int plane, int *r_linesize);
void MOV_yuv_frame_free(MovieYUVFrame &frame);
/** この糸の MOV_decode_frame_yuv で、GPU 上の面(NVDEC)を降ろさずに返す(layout 4 / 5)。 */
void MOV_prefer_device_for_thread(bool prefer_device);
/** GPU 上の面(layout 4 / 5)をメインメモリへ降ろして layout 1 / 2 にする(G2 が使えなかった時の非常口)。 */
bool MOV_yuv_frame_download(MovieYUVFrame &frame);
/**
 * GPU 上の面(layout 4 / 5)の 1 枚を、GPU のピクセルバッファへ GPU の中だけで写す。
 * `handle` は `GPU_pixel_buffer_get_native_handle()` の値: OpenGL ならバッファの名前、
 * Vulkan なら書き出した fd(`is_vulkan_fd`・この関数が受け取って閉じる)。
 * `row_bytes` × `rows` を詰めて書く。GPU の文脈が有効な糸から呼ぶこと。
 */
bool MOV_yuv_plane_copy_to_gpu_buffer(const MovieYUVFrame &frame,
                                      int plane,
                                      int64_t handle,
                                      size_t handle_size,
                                      bool is_vulkan_fd,
                                      int row_bytes,
                                      int rows);

/**
 * Falcon: この糸の MOV_decode_frame で、8bit を超える動画も 8bit の絵(byte)で返す。
 * VSE のプレビュー用(書き出しには使わない)。浮動小数の絵は 1 コマ 32MB・変換も重いので、
 * 再生が追いつかずメモリも溢れる(2026-09-22 実測: 10bit 16〜26fps・+8GB 対 8bit 60fps・+1GB)。
 */
void MOV_prefer_byte_for_thread(bool prefer_byte);

/**
 * Falcon: 上の指定で 8bit へ落とした絵を作ったことがあるか(読むと倒れる)。
 * 書き出しの前にキャッシュを捨てる合図に使う(8bit の絵が書き出しに混ざらないように)。
 */
bool MOV_take_byte_downgrade_happened();

/**
 * Fetches a frame from a movie used for preview/thumbnails.
 * The frame will be halfway into the file duration.
 * Thumbnail related metadata ("Thumb::Video::*") will be set on the
 * returned image.
 */
ImBuf *MOV_decode_preview_frame(MovieReader *anim);

/**
 * Returns the number of video streams in the movie file backing `anim`.
 * Returns 0 if the file cannot be opened or contains no video streams.
 */
int MOV_get_video_stream_count(MovieReader *anim);

/**
 * Return the length (in frames) of the movie.
 */
int MOV_get_duration_frames(const MovieReader *anim);

/**
 * Return the encoded start offset (in seconds) of the movie.
 */
double MOV_get_start_offset_seconds(const MovieReader *anim);

/**
 * Returns the frames per second of the movie, or zero if
 * the information is not available. Note that if you want the
 * most accurate representation, use #MOV_get_fps_num_denom.
 */
float MOV_get_fps(const MovieReader *anim);

/**
 * Returns the frames per second of the movie as numerator and
 * denominator. False will be returned if the information is
 * not available.
 */
bool MOV_get_fps_num_denom(const MovieReader *anim, short &r_fps_num, float &r_fps_denom);

/**
 * Get movie image width in pixels.
 */
int MOV_get_image_width(const MovieReader *anim);

/**
 * Get movie image height in pixels.
 */
int MOV_get_image_height(const MovieReader *anim);

/**
 * Returns true if movie playback has been fully initialized
 * and is supported. Note that immediately after #MOV_open_file
 * the playback is not initialized yet.
 */
bool MOV_is_initialized_and_valid(const MovieReader *anim);

/**
 * Gets filename (without the folder) part of the movie.
 */
void MOV_get_filename(const MovieReader *anim, char *filename, int filename_maxncpy);

/**
 * Loads metadata of the movie.
 * Metadata is only loaded for already initialized movies.
 */
IDProperty *MOV_load_metadata(MovieReader *anim);

/**
 * Sets multi-view suffix to be used when building proxies for this movie.
 */
void MOV_set_multiview_suffix(MovieReader *anim, const char *suffix);

/* -------------------------------------------------------------------- */
/** \name Movie proxy related functionality
 * \{ */

/**
 * Close any internally opened proxies of this movie.
 */
void MOV_close_proxies(MovieReader *anim);

/**
 * Custom directory to be used for loading or building proxies.
 * By default "BL_proxy" within the directory of the movie file is used.
 */
void MOV_set_custom_proxy_dir(MovieReader *anim, const char *dir);

/**
 * Queries which proxies exist for this movie.
 *
 * Note that it does not check whether proxies are up to date,
 * or valid files; just merely whether the expected files exist.
 *
 * Returns bitmask of #IMB_Proxy_Size flags.
 */
int MOV_get_existing_proxies(const MovieReader *anim);

/**
 * Initialize movie proxies builder.
 */
MovieProxyBuilder *MOV_proxy_builder_start(MovieReader *anim,
                                           int proxy_sizes_in_use,
                                           int quality,
                                           const bool overwrite,
                                           Set<std::string> *processed_paths,
                                           bool build_only_on_bad_performance);

/**
 * Will rebuild all used proxies at once.
 */
void MOV_proxy_builder_process(MovieProxyBuilder *context,
                               const bool *stop,
                               bool *do_update,
                               blender::FunctionRef<void(float progress)> set_progress_fn);

/**
 * Finish building proxies, and delete the builder.
 */
void MOV_proxy_builder_finish(MovieProxyBuilder *context, bool stop);

/** \} */

}  // namespace blender
