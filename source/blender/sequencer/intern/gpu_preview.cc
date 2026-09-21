/* SPDX-FileCopyrightText: 2026 Falcon Engine
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup sequencer
 */

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "DNA_scene_types.h"
#include "DNA_sequence_types.h"

#include "BKE_idprop.hh"
#include "BKE_scene.hh"

#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_string.h"
#include "BLI_time.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "GPU_framebuffer.hh"
#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_shader_builtin.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"

#include "modifiers/brightcontrast_cpu.hh"

#include "SEQ_falcon_timing.hh"
#include "SEQ_gpu_preview.hh"
#include "SEQ_iterator.hh"
#include "SEQ_render.hh"
#include "SEQ_sequencer.hh"

#include "prefetch.hh"
#include "render.hh"

namespace blender::seq {

/* -------------------------------------------------------------------- */
/** \name 有効・無効
 * \{ */

static bool gpu_preview_init()
{
  const char *env = getenv("FALCON_VSE_GPU_PREVIEW");
  return env != nullptr && env[0] != '\0' && strcmp(env, "0") != 0;
}

/* ★2026-09-21 作者「再生を GPU にまわせる? それかオンオフで切り替え可能にしたい」。
 * 起動時に 1 回読むだけだった値を、**その場で切り替えられる**形にした。環境変数は
 * 「場面がまだ何も持っていない時の既定」に下がる。保存はチャンネル数・上下反転と同じ
 * 場面の system property なので DNA は増えない。 */
#define FALCON_GPU_PREVIEW_PROP "falcon_vse_gpu_preview"

static bool &gpu_preview_runtime()
{
  static bool on = gpu_preview_init();
  return on;
}

bool gpu_preview_enabled()
{
  return gpu_preview_runtime();
}

/** この実行ファイルの VSE の色補正が、どの命令セットで建てられたか。
 * 段の印は CMake が `FALCON_VSE_TIER_*` で渡す(この翻訳単位は bf_sequencer の中なので、
 * 段ごとのコンパイル指定がそのまま効いている)。**建てる時に決まり、実行時には変わらない。** */
/* ★拡張命令の選び方も場面に持つ(2026-09-21)。0=自動 1=携帯 2=AVX2 3=参照。 */
#define FALCON_CPU_KERNEL_PROP "falcon_vse_cpu_kernel"

int falcon_cpu_kernel_get()
{
  using M = brightcontrast_cpu::Mode;
  switch (brightcontrast_cpu::mode()) {
    case M::Portable:
      return 1;
    case M::AVX2:
      return 2;
    case M::Reference:
      return 3;
    default:
      return 0;
  }
}

void falcon_cpu_kernel_store(Scene *scene, const int value)
{
  using M = brightcontrast_cpu::Mode;
  const M m = (value == 1) ? M::Portable :
              (value == 2) ? M::AVX2 :
              (value == 3) ? M::Reference :
                             M::Auto;
  brightcontrast_cpu::mode_set(m);
  if (scene == nullptr) {
    return;
  }
  IDProperty *group = IDP_ID_system_properties_ensure(&scene->id);
  IDProperty *prop = IDP_GetPropertyTypeFromGroup(group, FALCON_CPU_KERNEL_PROP, IDP_INT);
  if (prop != nullptr) {
    IDP_int_set(prop, value);
    return;
  }
  IDP_AddToGroup(group, bke::idprop::create(FALCON_CPU_KERNEL_PROP, value).release());
}

void falcon_cpu_kernel_sync_from_scene(const Scene *scene)
{
  if (scene == nullptr || scene->id.system_properties == nullptr) {
    return;
  }
  const IDProperty *prop = IDP_GetPropertyTypeFromGroup(
      scene->id.system_properties, FALCON_CPU_KERNEL_PROP, IDP_INT);
  if (prop != nullptr) {
    falcon_cpu_kernel_store(nullptr, IDP_int_get(prop));
  }
}

bool falcon_cpu_has_avx2()
{
  return brightcontrast_cpu::avx2_available();
}

const char *falcon_simd_tier()
{
#if defined(FALCON_VSE_TIER_AVX512)
  return "AVX-512";
#elif defined(FALCON_VSE_TIER_AVX2)
  return "AVX2+FMA";
#elif defined(FALCON_VSE_TIER_O3)
  return "O3 (auto-vectorized)";
#else
  return "Default";
#endif
}

void gpu_preview_set(const bool enable)
{
  gpu_preview_runtime() = enable;
}

void gpu_preview_sync_from_scene(const Scene *scene)
{
  if (scene == nullptr || scene->id.system_properties == nullptr) {
    return;
  }
  const IDProperty *prop = IDP_GetPropertyTypeFromGroup(
      scene->id.system_properties, FALCON_GPU_PREVIEW_PROP, IDP_INT);
  if (prop != nullptr) {
    gpu_preview_set(IDP_int_get(prop) != 0);
  }
}

void gpu_preview_store(Scene *scene, const bool enable)
{
  gpu_preview_set(enable);
  if (scene == nullptr) {
    return;
  }
  IDProperty *group = IDP_ID_system_properties_ensure(&scene->id);
  const int value = enable ? 1 : 0;
  IDProperty *prop = IDP_GetPropertyTypeFromGroup(group, FALCON_GPU_PREVIEW_PROP, IDP_INT);
  if (prop != nullptr) {
    IDP_int_set(prop, value);
    return;
  }
  IDP_AddToGroup(group, bke::idprop::create(FALCON_GPU_PREVIEW_PROP, value).release());
}

/** 転送を PixelBuffer 経由にするか(`FALCON_VSE_GPU_PBO`・**既定は入**・`=0` で今までの同期転送)。
 *
 * ★`GPU_texture_update()` は同期の転送で、ドライバが自前の中継バッファへ写してから送る。
 * PixelBuffer(OpenGL の PBO)は**GPU が見えるメモリを直接貸してくれる**ので、
 * その中継が 1 回減る。
 *
 * ★実測(2026-09-20・各 2 巡・1 枚あたりの中央値):
 *   FHD 1.43 → 1.18ms(1.21 倍)/ WQHD 5.32 → 4.05ms(1.31 倍)/ 4K 12.62 → 8.58ms(**1.47 倍**)。
 *   **大きいほど効く**(ドライバの中継コピーが画素数に比例するため)。負ける条件は見つからなかったので既定で入れる。 */
static bool gpu_preview_pbo()
{
  static const bool on = []() {
    const char *env = getenv("FALCON_VSE_GPU_PBO");
    if (env == nullptr || env[0] == '\0') {
      return true;
    }
    return strcmp(env, "0") != 0;
  }();
  return on;
}

int gpu_preview_ahead_frames()
{
  static const int n = []() {
    const char *env = getenv("FALCON_VSE_GPU_AHEAD");
    if (env == nullptr || env[0] == '\0') {
      /* ★既定は 0 = 先読みの側では作らない(段 2a の形)。
       * 実測(2026-09-20): 先読みの側で合成すると、復号と転送が**同じ 1 本のスレッドに並ぶ**。
       * WQHD 4 本なら 36ms + 21ms = 57ms で、画面を描く側と分けた時(36ms と 23ms が並ぶ)より遅い。
       * おまけに輪に留めるため先読みの窓を狭める必要があり、読み先が尽きて**画面側が同期復号に落ちた**
       * (10 秒で 1.8fps)。Blender の先読みは 1 本しかないので、この形は割に合わない。
       * 残してあるのは、復号が GPU へ移った後(段 3)に効く可能性があるため。 */
      return 0;
    }
    const int v = atoi(env);
    return v < 0 ? 0 : v;
  }();
  return n;
}

/** 直近の 1 コマで GPU 経路が通ったか(先読み側が読む)。 */
static std::atomic<bool> g_gpu_preview_active{false};

bool gpu_preview_is_active()
{
  return g_gpu_preview_active.load(std::memory_order_relaxed);
}

static gpu::Texture *gpu_preview_give_up()
{
  g_gpu_preview_active.store(false, std::memory_order_relaxed);
  return nullptr;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name 通せる配置かどうか
 *
 * ★ここを外した物が 1 本でもあれば、丸ごと今までの CPU 経路へ落とす。
 * 「半分だけ GPU」は絵が合わなくなるので作らない。
 * \{ */

static bool filter_is_supported(const Strip *strip, int *r_use_linear)
{
  const StripTransform *transform = strip->data->transform;
  int filter = transform->filter;

  if (filter == SEQ_TRANSFORM_FILTER_AUTO) {
    /* `get_auto_filter()`(render.cc)と同じ判断。ここだけは写しになる。 */
    const float sx = fabsf(transform->scale_x);
    const float sy = fabsf(transform->scale_y);
    if (sx > 2.0f && sy > 2.0f) {
      filter = SEQ_TRANSFORM_FILTER_CUBIC_MITCHELL;
    }
    else if (sx < 0.5f && sy < 0.5f) {
      filter = SEQ_TRANSFORM_FILTER_BOX;
    }
    else if (sx == 1.0f && sy == 1.0f && roundf(transform->xofs) == transform->xofs &&
             roundf(transform->yofs) == transform->yofs && transform->rotation == 0.0f)
    {
      filter = SEQ_TRANSFORM_FILTER_NEAREST;
    }
    else {
      filter = SEQ_TRANSFORM_FILTER_BILINEAR;
    }
  }

  switch (filter) {
    case SEQ_TRANSFORM_FILTER_NEAREST:
      *r_use_linear = 0;
      return true;
    case SEQ_TRANSFORM_FILTER_BILINEAR:
      *r_use_linear = 1;
      return true;
    default:
      /* Box と Cubic は GPU の標本器では作れない(専用の着色器が要る)。まだ通さない。 */
      return false;
  }
}

static bool strip_is_eligible(const Strip *strip, const bool is_bottom, int *r_use_linear)
{
  if (!ELEM(strip->type, STRIP_TYPE_MOVIE, STRIP_TYPE_IMAGE)) {
    return false;
  }
  if (strip->modifiers.first != nullptr) {
    return false;
  }
  if ((strip->flag & (SEQ_DEINTERLACE | SEQ_MAKE_FLOAT)) != 0) {
    return false;
  }
  if (strip->sat != 1.0f || strip->mul != 1.0f) {
    return false;
  }
  if (sequencer_use_crop(strip)) {
    return false;
  }
  if (strip->blend_mode == STRIP_BLEND_REPLACE) {
    /* Replace は下ごしらえで `mul *= 不透明度` になる。100% の時だけ通す。 */
    if (!is_bottom || strip->blend_opacity != 100.0f) {
      return false;
    }
  }
  else if (strip->blend_mode != STRIP_BLEND_ALPHAOVER) {
    return false;
  }
  return filter_is_supported(strip, r_use_linear);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name 素材を集める(CPU 側・GPU 文脈は要らない)
 * \{ */

struct GpuLayer {
  ImBuf *image = nullptr;
  float3x3 matrix = float3x3::identity();
  float factor = 1.0f;
  bool use_linear = false;
};

static void gpu_layers_free(Vector<GpuLayer> &layers)
{
  for (GpuLayer &layer : layers) {
    IMB_freeImBuf(layer.image);
    layer.image = nullptr;
  }
  layers.clear();
}

/** 通せるなら素材をそろえて true。1 本でも外れたら何も残さず false。 */
static bool gpu_preview_collect(const RenderData *context,
                                const float timeline_frame,
                                const int chanshown,
                                Vector<GpuLayer> &r_layers,
                                const char **r_colorspace)
{
  *r_colorspace = nullptr;

  Scene *scene = context->scene;
  Editing *ed = editing_get(scene);
  if (ed == nullptr || context->rectx <= 0 || context->recty <= 0) {
    return false;
  }
  if (context->render != nullptr || context->gpu_offscreen != nullptr) {
    /* 書き出しは対象外(画面に出す 1 枚だけの経路)。 */
    return false;
  }

  ListBaseT<Strip> *seqbasep = ed->current_strips();
  ListBaseT<SeqTimelineChannel> *channels = ed->current_channels();

  Vector<Strip *> strips = query_rendered_strips_sorted(
      scene, channels, seqbasep, timeline_frame, chanshown);
  if (strips.is_empty()) {
    return false;
  }

  /* まず全部の可否を見る。1 本でも外れたら**復号もせずに**諦める。 */
  Vector<int> use_linear(strips.size(), 0);
  for (const int64_t i : strips.index_range()) {
    int linear = 0;
    if (!strip_is_eligible(strips[i], i == 0, &linear)) {
      return false;
    }
    use_linear[i] = linear;
  }

  SeqRenderState state;
  state.is_current_frame = timeline_frame == BKE_scene_frame_get(scene);

  const char *colorspace = nullptr;
  r_layers.reserve(strips.size());

  for (const int64_t i : strips.index_range()) {
    Strip *strip = strips[i];
    bool is_proxy_image = false;
    SeqResult res = seq_render_strip_source_only(
        context, &state, strip, timeline_frame, &is_proxy_image);
    if (!res.is_valid()) {
      gpu_layers_free(r_layers);
      return false;
    }
    /* 8bit の素材だけ。float の素材はまだ通さない。 */
    if (res.image->byte_data() == nullptr || res.image->float_data() != nullptr ||
        res.translation != float2(0, 0) ||
        /* ★アルファを持ちうる素材は通さない。GPU 側は「アルファを掛けた形」で重ねるので、
         * 素材が不透明でないと CPU と式が変わる。 */
        res.image->can_contain_alpha())
    {
      IMB_freeImBuf(res.image);
      gpu_layers_free(r_layers);
      return false;
    }
    const char *cs = IMB_colormanagement_get_byte_colorspace(res.image);
    if (colorspace == nullptr) {
      colorspace = cs;
    }
    else if (cs == nullptr || !STREQ(cs, colorspace)) {
      /* 色空間が混ざる配置は通さない(1 枚のテクスチャに 1 つしか名前を付けられない)。 */
      IMB_freeImBuf(res.image);
      gpu_layers_free(r_layers);
      return false;
    }

    const float preview_scale_factor = get_render_scale_factor(*context);
    const bool do_scale_to_render_size = seq_need_scale_to_render_size(strip, is_proxy_image);
    const float image_scale_factor = do_scale_to_render_size ? preview_scale_factor : 1.0f;

    GpuLayer layer;
    layer.image = res.image;
    layer.matrix = calc_strip_transform_matrix(scene,
                                               strip,
                                               int2(res.image->x, res.image->y),
                                               int2(context->rectx, context->recty),
                                               image_scale_factor,
                                               preview_scale_factor);
    layer.factor = strip->blend_opacity / 100.0f;
    layer.use_linear = use_linear[i] != 0;
    r_layers.append(layer);
  }

  if (r_layers.is_empty()) {
    return false;
  }
  *r_colorspace = colorspace;
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name 合成(GPU 文脈が有効な所でだけ呼ぶ)
 * \{ */

static gpu::Texture *gpu_preview_composite(const RenderData *context,
                                           Vector<GpuLayer> &layers,
                                           const bool prefetch)
{
  /* 仕上がりの置き場。半精度の float に、**アルファを掛けた形**で 8bit の値をそのまま積む
   * (色空間は素材のまま。表示側が最後に画面の色空間へ直す)。 */
  gpu::Texture *out = GPU_texture_create_2d("falcon_vse_gpu_preview",
                                            context->rectx,
                                            context->recty,
                                            1,
                                            gpu::TextureFormat::SFLOAT_16_16_16_16,
                                            GPU_TEXTURE_USAGE_SHADER_READ |
                                                GPU_TEXTURE_USAGE_ATTACHMENT,
                                            nullptr);
  if (out == nullptr) {
    gpu_layers_free(layers);
    return nullptr;
  }

  gpu::FrameBuffer *prev_fb = GPU_framebuffer_active_get();
  gpu::FrameBuffer *fb = nullptr;
  GPU_framebuffer_ensure_config(&fb, {GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(out)});
  GPU_framebuffer_bind(fb);
  GPU_framebuffer_clear_color(fb, double4(0.0, 0.0, 0.0, 0.0));

  GPU_matrix_push_projection();
  GPU_matrix_ortho_set(0.0f, float(context->rectx), 0.0f, float(context->recty), -1.0f, 1.0f);
  GPU_matrix_push();
  GPU_matrix_identity_set();

  const GPUBlend prev_blend = GPU_blend_get();
  GPU_blend(GPU_BLEND_ALPHA_PREMULT);

  GPUVertFormat *format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);
  const uint texco = GPU_vertformat_attr_add(format, "texCoord", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_IMAGE_COLOR);

  /* ★素材のテクスチャは 1 コマのあいだ全部持ったままにする。
   * 使い回しの入れ物(TexturePool)から取ると、1 枚目に描いた直後の同じ入れ物が
   * 2 枚目に返ってきて、GPU がまだ読んでいる最中の入れ物へ書き込むことになる。
   * その待ちで 1 枚 1.7ms のはずの転送が 3.6ms になっていた(2026-09-20 実測)。 */
  Vector<gpu::Texture *> src_textures(layers.size(), nullptr);

  for (const int64_t li : layers.index_range()) {
    GpuLayer &layer = layers[li];
    const int w = layer.image->x;
    const int h = layer.image->y;

    gpu::Texture *tex = nullptr;
    {
      /* ★ここが「CPU から GPU へ送る」時間。段 2 の勝ち負けはこの値で決まる。 */
      timing::Scope timer(timing::Stage::Upload, prefetch);
      tex = GPU_texture_create_2d("falcon_vse_gpu_src",
                                  w,
                                  h,
                                  1,
                                  gpu::TextureFormat::UNORM_8_8_8_8,
                                  GPU_TEXTURE_USAGE_SHADER_READ,
                                  nullptr);
      if (tex != nullptr) {
        bool sent = false;
        if (gpu_preview_pbo()) {
          /* GPU が見えるメモリを借りて、そこへ直接書いてから GPU 側だけで写す。
           * ドライバの中継バッファへの 1 回が減る。 */
          const size_t byte_size = size_t(w) * size_t(h) * 4;
          GPUPixelBuffer *pix_buf = GPU_pixel_buffer_create(byte_size);
          if (pix_buf != nullptr) {
            void *dst = GPU_pixel_buffer_map(pix_buf);
            if (dst != nullptr) {
              memcpy(dst, layer.image->byte_data(), byte_size);
              GPU_pixel_buffer_unmap(pix_buf);
              GPU_texture_update_sub_from_pixel_buffer(
                  tex, GPU_DATA_UBYTE, pix_buf, 0, 0, 0, w, h, 1);
              sent = true;
            }
            GPU_pixel_buffer_free(pix_buf);
          }
        }
        if (!sent) {
          GPU_texture_update(tex, GPU_DATA_UBYTE, layer.image->byte_data());
        }
      }
    }
    src_textures[li] = tex;
    if (tex == nullptr) {
      continue;
    }

    {
      timing::Scope timer(timing::Stage::GpuComposite, prefetch);

      const GPUSamplerState sampler = {layer.use_linear ? GPU_SAMPLER_FILTERING_LINEAR :
                                                          GPU_SAMPLER_FILTERING_DEFAULT,
                                       GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER,
                                       GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER,
                                       GPU_SAMPLER_CUSTOM_COMPARE,
                                       GPU_SAMPLER_STATE_TYPE_PARAMETERS};
      immBindTextureSampler("image", tex, sampler);

      /* アルファを掛けた形での「重ね」= 出力 = fac*上 + (1 - fac*上のα)*下。
       * 素材は不透明なので「掛けた形」と「掛けていない形」は同じ値になる(可否判定で担保)。
       * 端の 1 画素だけは標本器が縁(透明)と混ぜるので、そこは掛けた形が出る。 */
      immUniformColor4f(layer.factor, layer.factor, layer.factor, layer.factor);

      /* 素材の外側 1 画素ぶん広げて描く。CPU 側(`IMB_transform` の CROP_SRC)が
       * 端で透明と混ぜるのと同じ見え方にするため。 */
      const float mx = 1.0f;
      const float2 src[4] = {float2(-mx, -mx),
                             float2(float(w) + mx, -mx),
                             float2(float(w) + mx, float(h) + mx),
                             float2(-mx, float(h) + mx)};
      const float2 uv[4] = {float2(-mx / w, -mx / h),
                            float2((w + mx) / w, -mx / h),
                            float2((w + mx) / w, (h + mx) / h),
                            float2(-mx / w, (h + mx) / h)};

      immBegin(GPU_PRIM_TRI_FAN, 4);
      for (int c = 0; c < 4; c++) {
        const float2 p = math::transform_point(layer.matrix, src[c]);
        immAttr2f(texco, uv[c].x, uv[c].y);
        immVertex2f(pos, p.x, p.y);
      }
      immEnd();

      GPU_texture_unbind(tex);
    }

    IMB_freeImBuf(layer.image);
    layer.image = nullptr;
  }

  immUnbindProgram();
  for (gpu::Texture *tex : src_textures) {
    if (tex != nullptr) {
      GPU_texture_free(tex);
    }
  }
  layers.clear();

  GPU_blend(prev_blend);
  GPU_matrix_pop();
  GPU_matrix_pop_projection();

  if (prev_fb != nullptr) {
    GPU_framebuffer_bind(prev_fb);
  }
  else {
    GPU_framebuffer_restore();
  }
  GPU_framebuffer_free(fb);

  /* ★表示側の標本の仕方を CPU 経路(`create_texture()`)と合わせる。
   * 合わせないと、プレビューを縮小表示している時だけ片方が滑らかになって絵が変わる
   * (2026-09-20: これを忘れて最大差 102 段の食い違いが出た)。 */
  GPU_texture_filter_mode(out, false);
  return out;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name 仕上がりの輪(先読みの側が作って、画面を描く側が使う)
 *
 * ★VRAM に置くのは**仕上がり 1 枚だけ**を数コマぶん。素材を GPU に溜めるのに比べて桁が小さい
 * (FHD 11MB / 4K 44MB ×コマ数)。
 * \{ */

struct GpuFrameItem {
  int timeline_frame = -1;
  int view_id = -1;
  int chanshown = -1;
  int width = -1;
  int height = -1;
  int64_t last_used = -1;
  gpu::Texture *texture = nullptr;
  const char *colorspace = nullptr;

  bool matches(const int frame, const int view, const int chan, const int w, const int h) const
  {
    return this->texture != nullptr && this->timeline_frame == frame && this->view_id == view &&
           this->chanshown == chan && this->width == w && this->height == h;
  }
};

/** ★輪の大きさは「何コマ先まで作るか」と同じ。作った物がすぐ捨てられては意味がない。 */
static constexpr int GPU_FRAME_RING_MAX = 16;

struct GpuFrameRing {
  std::mutex mutex;
  GpuFrameItem items[GPU_FRAME_RING_MAX];
  int64_t tick = 0;
  /** 画面を描く側が最後に求めたコマ。先読みが走りすぎないための手綱。 */
  std::atomic<int> last_wanted{-1};
};

static GpuFrameRing g_ring;

/** ★GPU 文脈が有効な所からだけ呼ぶこと(テクスチャを手放すため)。 */
void gpu_preview_ring_clear()
{
  std::lock_guard<std::mutex> lock(g_ring.mutex);
  for (GpuFrameItem &item : g_ring.items) {
    if (item.texture != nullptr) {
      GPU_texture_free(item.texture);
      item.texture = nullptr;
    }
    item.timeline_frame = -1;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name 先読みの側(別スレッド・副 GPU 文脈)
 * \{ */

/** 先読み側が作れなかった理由を数える(`FALCON_VSE_GPU_DEBUG=1` の時だけ出す)。 */
static bool gpu_preview_debug()
{
  static const bool on = []() {
    const char *env = getenv("FALCON_VSE_GPU_DEBUG");
    return env != nullptr && env[0] != '\0' && strcmp(env, "0") != 0;
  }();
  return on;
}

static std::atomic<int> g_why[6];

static bool gpu_preview_no(const int why)
{
  if (gpu_preview_debug()) {
    const int n = g_why[why].fetch_add(1, std::memory_order_relaxed) + 1;
    if (n % 50 == 1) {
      printf("{\"k\":\"gpu_produce_no\",\"why\":%d,\"n\":%d}\n", why, n);
      fflush(stdout);
    }
  }
  return false;
}

bool gpu_preview_produce(const RenderData *context, const float timeline_frame, const int chanshown)
{
  if (!gpu_preview_enabled() || gpu_preview_ahead_frames() == 0) {
    return gpu_preview_no(0);
  }

  const int frame = int(timeline_frame);
  const int wanted = g_ring.last_wanted.load(std::memory_order_relaxed);
  if (wanted >= 0) {
    const int ahead = frame - wanted;
    /* 走りすぎない・戻りすぎない。輪に入らないコマを作っても押し出されるだけ。 */
    if (ahead < 0 || ahead > gpu_preview_ahead_frames()) {
      if (gpu_preview_debug()) {
        const int n = g_why[1].fetch_add(1, std::memory_order_relaxed) + 1;
        if (n % 50 == 1) {
          printf("{\"k\":\"gpu_produce_no\",\"why\":1,\"n\":%d,\"frame\":%d,\"wanted\":%d}\n",
                 n, frame, wanted);
          fflush(stdout);
        }
      }
      return false;
    }
  }

  /* 既にあるなら作らない。 */
  {
    std::lock_guard<std::mutex> lock(g_ring.mutex);
    for (GpuFrameItem &item : g_ring.items) {
      if (item.matches(frame, context->view_id, chanshown, context->rectx, context->recty)) {
        return true;
      }
    }
  }

  Vector<GpuLayer> layers;
  const char *colorspace = nullptr;
  if (!gpu_preview_collect(context, timeline_frame, chanshown, layers, &colorspace)) {
    return gpu_preview_no(2);
  }

  const GpuContextState state = render_begin_gpu(*context);
  if (state == GpuContextState::Unsupported) {
    /* GPU が使えないスレッドなら、素材だけ作って終わり(復号は無駄にならない)。 */
    gpu_layers_free(layers);
    return gpu_preview_no(3);
  }
  if (gpu_preview_debug()) {
    const int n = g_why[4].fetch_add(1, std::memory_order_relaxed) + 1;
    if (n % 50 == 1) {
      printf("{\"k\":\"gpu_produce_ok\",\"n\":%d,\"ctx\":%d}\n", n, int(state));
      fflush(stdout);
    }
  }

  gpu::Texture *texture = gpu_preview_composite(context, layers, true);

  if (texture != nullptr) {
    /* ★別の文脈から読ませるので、ここで完了させておく。 */
    GPU_finish();

    std::lock_guard<std::mutex> lock(g_ring.mutex);
    /* ★画面を描く側がいま求めているコマは押し出さない。
     * 借りたテクスチャを描いている最中に手放されると、絵が壊れるか落ちる。 */
    const int in_use = g_ring.last_wanted.load(std::memory_order_relaxed);
    GpuFrameItem *slot = nullptr;
    for (GpuFrameItem &item : g_ring.items) {
      if (item.texture == nullptr) {
        slot = &item;
        break;
      }
      if (item.timeline_frame == in_use) {
        continue;
      }
      if (slot == nullptr || item.last_used < slot->last_used) {
        slot = &item;
      }
    }
    if (slot == nullptr) {
      /* 押し出せる場所が無い = 作った物は捨てる(輪が全部使用中という稀な形)。 */
      GPU_texture_free(texture);
      texture = nullptr;
    }
    else
    {
      if (slot->texture != nullptr) {
        GPU_texture_free(slot->texture);
      }
      slot->texture = texture;
      slot->timeline_frame = frame;
      slot->view_id = context->view_id;
      slot->chanshown = chanshown;
      slot->width = context->rectx;
      slot->height = context->recty;
      slot->colorspace = colorspace;
      slot->last_used = ++g_ring.tick;
    }
  }

  render_end_gpu(*context, state);
  return texture != nullptr;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name 画面を描く側(主スレッド)
 * \{ */

gpu::Texture *gpu_preview_render(const RenderData *context,
                                 const float timeline_frame,
                                 const int chanshown,
                                 const char **r_colorspace_name,
                                 bool *r_owned)
{
  *r_colorspace_name = nullptr;
  *r_owned = false;

  /* 1 コマの壁時計(通常の経路の `render_give_ibuf()` と同じ役目)。
   * GPU 経路ではプレビューが `render_give_ibuf()` を通らないので、ここで出さないと
   * 転送と合成の時間がどの行にも出てこない。 */
  const double timing_start = timing::enabled() ? BLI_time_now_seconds() : 0.0;

  const int frame = int(timeline_frame);
  g_ring.last_wanted.store(frame, std::memory_order_relaxed);

  /* ★素材をキャッシュへ入れる前に、上限に当たっていれば追い出す。
   * 通常の経路は `render_give_ibuf()` が 1 コマごとにここを通っているが、GPU 経路は通らない。
   * 先読みの側は通っているので上限そのものは守られるが、先読みが止まっている間は
   * 画面側の格納だけが進む。通常の経路と同じ形にそろえる。
   * ★先読みの側では呼ばない — あちらは `render_give_ibuf()` の中で既に錠を取っているので、
   * ここで取ると自分で自分を待つ。 */
  seq_render_evict_caches_if_full(context);

  /* ①先読みの側が作ってくれていれば、それを描くだけ(転送も合成も要らない)。 */
  {
    std::lock_guard<std::mutex> lock(g_ring.mutex);
    for (GpuFrameItem &item : g_ring.items) {
      if (item.matches(frame, context->view_id, chanshown, context->rectx, context->recty)) {
        item.last_used = ++g_ring.tick;
        g_gpu_preview_active.store(true, std::memory_order_relaxed);
        seq_prefetch_start(context, timeline_frame);
        if (timing_start != 0.0) {
          timing::frame_done(frame, BLI_time_now_seconds() - timing_start, false);
        }
        *r_colorspace_name = item.colorspace;
        return item.texture;
      }
    }
  }

  /* ②無ければこの場で作る。 */
  Vector<GpuLayer> layers;
  const char *colorspace = nullptr;
  if (!gpu_preview_collect(context, timeline_frame, chanshown, layers, &colorspace)) {
    return gpu_preview_give_up();
  }

  gpu::Texture *out = gpu_preview_composite(context, layers, false);
  if (out == nullptr) {
    return gpu_preview_give_up();
  }

  /* 先読みを起こす(通常の `render_give_ibuf()` と同じ役目。ここを忘れると読み先が無くなり、
   * 復号が全部この場(描画の最中)で起きて逆に遅くなる)。 */
  seq_prefetch_start(context, timeline_frame);

  g_gpu_preview_active.store(true, std::memory_order_relaxed);
  if (timing_start != 0.0) {
    timing::frame_done(frame, BLI_time_now_seconds() - timing_start, false);
  }
  *r_colorspace_name = colorspace;
  *r_owned = true;
  return out;
}

/** \} */

}  // namespace blender::seq
