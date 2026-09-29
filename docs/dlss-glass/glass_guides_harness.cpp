// SPDX-License-Identifier: Apache-2.0
//
// Falcon: 「ガラス越しガイド」(DLSS-RR)の CPU 検証ハーネス。
//
// Cycles の CPU カーネルをそのまま取り込み(`kernel/device/cpu/kernel.cpp`)、合成した交点データと
// BSDF クロージャで、ガイドの書き込み(`film_write_denoising_features_surface` と
// `film_write_denoising_glass_resolve`)を動かして、フラグと書き込み値を確かめる。
// GPU も Blender 本体のビルドも要らない。実行は `docs/dlss-glass/run_harness.sh`。
//
// 見ているのは「ロジックが意図どおり動くか」だけで、絵の良し悪しは実機の A/B で見る
// (docs/dlss-glass-guides.md)。

// Standalone check of the glass through-guides logic. Includes the CPU kernel TU as is.
#include "kernel/device/cpu/kernel.cpp"
#include <cstdio>
#include <cstring>
#include <vector>
#include <new>
#include <cstdlib>

using namespace ccl;

namespace ccl {
bool system_cpu_support_avx2() { return false; }
void transform_inverse_cpu_avx2(const Transform &, Transform &) {}
}  // namespace ccl

static int g_fail = 0;
static int g_level = 0; /* glass smooth level (bits 5..7 of the options) */
#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } else { printf("  ok:   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const int STRIDE = 32;
enum { P_DEPTH = 0, P_NORMAL = 1, P_ALBEDO = 4, P_SPEC_ALBEDO = 7, P_ROUGH = 10, P_HITDIST = 12 };

struct Ctx {
  ThreadKernelGlobalsCPU *kg;
  IntegratorStateCPU *state;
  std::vector<float> buf;
  ShaderData *sd;
};

static void setup(Ctx &c, bool gtg, bool follow = false, bool matte = false)
{
  KernelGlobalsCPU *kg = c.kg;
  memset((void *)&kg->data.film, 0, sizeof(KernelFilm));
  KernelFilm &f = kg->data.film;
  f.pass_stride = STRIDE;
  f.pass_denoising_depth = P_DEPTH;
  f.pass_denoising_normal = P_NORMAL;
  f.pass_denoising_albedo = P_ALBEDO;
  f.pass_denoising_specular_albedo = P_SPEC_ALBEDO;
  f.pass_denoising_roughness = P_ROUGH;
  f.pass_denoising_backward_motion = PASS_UNUSED;
  f.pass_denoising_specular_hit_distance = P_HITDIST;
  f.specular_hit_distance_far = 1e4f;
  f.denoising_pass_options_flag = (gtg ? DENOISING_PASS_GLASS_THROUGH : 0) |
                                  (matte ? DENOISING_PASS_GLASS_MATTE : 0) |
                                  (follow ? DENOISING_PASS_FOLLOW_REFLECTIONS : 0) |
                                  (g_level << DENOISING_PASS_GLASS_SMOOTH_SHIFT);
  kg->data.cam.type = CAMERA_PERSPECTIVE;
  kg->data.cam.worldtocamera = transform_identity();
  memset(c.state, 0, sizeof(IntegratorStateCPU));
  c.state->path.flag = PATH_RAY_DENOISING_FEATURES;
  c.state->path.bounce = 0;
  c.state->path.render_pixel_index = 0;
  c.state->path.denoising_feature_throughput = make_float3(1.0f, 1.0f, 1.0f);
  c.state->ray.tmin = 0.0f;
  std::fill(c.buf.begin(), c.buf.end(), 0.0f);
}

static void set_glass(ShaderData *sd, float depth_z, float alpha)
{
  memset((void *)sd, 0, sizeof(ShaderData));
  sd->P = make_float3(0, 0, depth_z);
  sd->N = sd->Ng = make_float3(0, 0, -1);
  sd->wi = make_float3(0, 0, -1);
  sd->ray_length = depth_z;
  sd->flag = SD_BSDF;
  sd->num_closure = 1;
  ShaderClosure *sc = &sd->closure[0];
  sc->type = CLOSURE_BSDF_MICROFACET_GGX_GLASS_ID;
  sc->weight = make_float3(1.0f, 1.0f, 1.0f);
  sc->sample_weight = 1.0f;
  sc->N = make_float3(0, 0, -1);
  MicrofacetBsdf *mb = (MicrofacetBsdf *)sc;
  mb->alpha_x = mb->alpha_y = alpha;
  mb->ior = 1.45f;
  mb->fresnel_type = MicrofacetFresnel::DIELECTRIC;
  mb->fresnel = nullptr;
  mb->energy_scale = 1.0f;
}

static void set_diffuse(ShaderData *sd, float depth_z, float3 albedo)
{
  memset((void *)sd, 0, sizeof(ShaderData));
  sd->P = make_float3(0, 0, depth_z);
  sd->N = sd->Ng = make_float3(0, 0, -1);
  sd->wi = make_float3(0, 0, -1);
  sd->ray_length = depth_z;
  sd->flag = SD_BSDF | SD_BSDF_HAS_EVAL;
  sd->num_closure = 1;
  ShaderClosure *sc = &sd->closure[0];
  sc->type = CLOSURE_BSDF_DIFFUSE_ID;
  sc->weight = albedo;
  sc->sample_weight = 1.0f;
  sc->N = make_float3(0, 0, -1);
}

static float *px(Ctx &c) { return c.buf.data(); }
static bool all_zero(Ctx &c)
{
  for (float v : c.buf) {
    if (v != 0.0f) return false;
  }
  return true;
}
static void dump(Ctx &c)
{
  float *b = px(c);
  printf("     depth=%.3f normal=(%.3f %.3f %.3f) albedo=(%.3f %.3f %.3f) spec=(%.3f %.3f %.3f) rough=%.3f hit=(%.3f %.3f)\n",
         b[P_DEPTH], b[P_NORMAL], b[P_NORMAL + 1], b[P_NORMAL + 2], b[P_ALBEDO], b[P_ALBEDO + 1], b[P_ALBEDO + 2],
         b[P_SPEC_ALBEDO], b[P_SPEC_ALBEDO + 1], b[P_SPEC_ALBEDO + 2], b[P_ROUGH], b[P_HITDIST], b[P_HITDIST + 1]);
  printf("     flags: DENOISING=%d PENDING=%d THROUGH=%d SEEN=%d  feature_thr=(%.2f %.2f %.2f)\n",
         !!(c.state->path.flag & PATH_RAY_DENOISING_FEATURES), !!(c.state->path.flag & PATH_RAY_GLASS_PENDING),
         !!(c.state->path.flag & PATH_RAY_GLASS_THROUGH), !!(c.state->path.flag & PATH_RAY_GLASS_SEEN),
         (float)c.state->path.denoising_feature_throughput.x, (float)c.state->path.denoising_feature_throughput.y,
         (float)c.state->path.denoising_feature_throughput.z);
}

int main()
{
  Ctx c;
  c.kg = (ThreadKernelGlobalsCPU *)calloc(1, sizeof(ThreadKernelGlobalsCPU));
  new ((void *)static_cast<KernelGlobalsCPU *>(c.kg)) KernelGlobalsCPU();
  c.state = new IntegratorStateCPU();
  c.sd = (ShaderData *)aligned_alloc(16, sizeof(ShaderData) + 64);
  c.buf.assign(STRIDE, 0.0f);

  /* Lookup tables for the dielectric albedo estimate: constant, enough for the 3 tables. */
  static std::vector<float> lut(16 * 16 * 16 * 4, 0.05f);
  c.kg->lookup_table.data = lut.data();
  c.kg->lookup_table.width = (int)lut.size();

  printf("== T0 disabled (stock behaviour): glass writes its own guides at the first hit ==\n");
  setup(c, false);
  set_glass(c.sd, 5.0f, 0.0f);
  film_write_denoising_features_surface(c.kg, c.state, c.sd, px(c), false);
  dump(c);
  CHECK(!(c.state->path.flag & PATH_RAY_GLASS_PENDING), "no PENDING flag when off");
  CHECK(!all_zero(c) && c.buf[P_DEPTH] > 4.9f && c.buf[P_DEPTH] < 5.1f, "glass depth written (%.2f)", c.buf[P_DEPTH]);
  CHECK(c.buf[P_ROUGH] == 0.0f, "roughness 0");
  float glass_spec = c.buf[P_SPEC_ALBEDO];

  printf("== T1 enabled: glass first hit is held back ==\n");
  setup(c, true);
  set_glass(c.sd, 5.0f, 0.0f);
  film_write_denoising_features_surface(c.kg, c.state, c.sd, px(c), false);
  dump(c);
  CHECK(all_zero(c), "nothing written yet");
  CHECK(c.state->path.flag & PATH_RAY_GLASS_PENDING, "PENDING set");
  CHECK(c.state->path.flag & PATH_RAY_DENOISING_FEATURES, "DENOISING_FEATURES kept");

  printf("== T2 resolve: sample REFLECTS -> glass surface guides now ==\n");
  film_write_denoising_glass_resolve(c.kg, c.state, c.sd, &c.sd->closure[0], LABEL_REFLECT | LABEL_SINGULAR, px(c));
  dump(c);
  CHECK(!(c.state->path.flag & PATH_RAY_GLASS_PENDING), "PENDING cleared");
  CHECK((c.state->path.flag & PATH_RAY_GLASS_SEEN) && !(c.state->path.flag & PATH_RAY_GLASS_THROUGH), "SEEN, not THROUGH");
  CHECK(c.buf[P_DEPTH] > 4.9f && c.buf[P_DEPTH] < 5.1f, "glass depth %.2f", c.buf[P_DEPTH]);
  CHECK(c.buf[P_SPEC_ALBEDO] == glass_spec, "same specular albedo as the stock path (%.3f)", c.buf[P_SPEC_ALBEDO]);
  CHECK(!(c.state->path.flag & PATH_RAY_DENOISING_FEATURES), "guide writing stops after the glass (like stock)");
  /* the reflected hit's specular hit distance must be suppressed */
  c.state->path.bounce = 1;
  c.state->path.glossy_bounce = 1;
  film_write_denoising_specular_hit_distance(c.kg, c.state, 3.0f, px(c));
  CHECK(c.buf[P_HITDIST] == 0.0f && c.buf[P_HITDIST + 1] == 0.0f, "no hit distance for glass paths");

  printf("== T3 resolve: sample REFRACTS -> nothing yet, next surface writes ==\n");
  setup(c, true);
  set_glass(c.sd, 5.0f, 0.0f);
  film_write_denoising_features_surface(c.kg, c.state, c.sd, px(c), false);
  film_write_denoising_glass_resolve(c.kg, c.state, c.sd, &c.sd->closure[0], LABEL_TRANSMIT | LABEL_SINGULAR, px(c));
  dump(c);
  CHECK(all_zero(c), "nothing written at the glass");
  CHECK((c.state->path.flag & PATH_RAY_GLASS_THROUGH) && (c.state->path.flag & PATH_RAY_GLASS_SEEN), "THROUGH+SEEN");
  CHECK(c.state->path.flag & PATH_RAY_DENOISING_FEATURES, "DENOISING_FEATURES kept");
  /* now the path is at bounce 1, on a diffuse wall 9 units away */
  c.state->path.bounce = 1;
  ShaderData *wall = (ShaderData *)aligned_alloc(16, sizeof(ShaderData) + 64);
  set_diffuse(wall, 9.0f, make_float3(0.5f, 0.4f, 0.3f));
  wall->ray_length = 4.0f; /* from the glass at z=5 */
  film_write_denoising_features_surface(c.kg, c.state, wall, px(c), false);
  dump(c);
  CHECK(fabsf(c.buf[P_DEPTH] - 9.0f) < 1e-3f, "wall's ABSOLUTE camera depth 9 (got %.3f)", c.buf[P_DEPTH]);
  CHECK(fabsf(c.buf[P_ALBEDO] - 0.5f) < 1e-3f && fabsf(c.buf[P_ALBEDO + 1] - 0.4f) < 1e-3f, "wall albedo written");
  CHECK(fabsf(c.buf[P_NORMAL + 2] + 1.0f) < 1e-3f, "wall normal written (%.2f)", c.buf[P_NORMAL + 2]);
  CHECK(c.buf[P_SPEC_ALBEDO] == 0.0f, "wall has no specular albedo");
  CHECK(!(c.state->path.flag & PATH_RAY_GLASS_THROUGH), "THROUGH consumed");
  CHECK(!(c.state->path.flag & PATH_RAY_DENOISING_FEATURES), "features stop after the wall");
  /* a later hit must not write anything more */
  float before = c.buf[P_DEPTH];
  c.state->path.bounce = 2;
  film_write_denoising_features_surface(c.kg, c.state, wall, px(c), false);
  CHECK(c.buf[P_DEPTH] == before, "no further writes");

  printf("== T4 through a tinted glass (lone closure): albedo = wall x glass colour ==\n");
  setup(c, true);
  set_glass(c.sd, 5.0f, 0.0f);
  c.sd->closure[0].weight = make_float3(0.9f, 0.6f, 0.3f);
  c.sd->closure[0].sample_weight = 0.6f;
  film_write_denoising_features_surface(c.kg, c.state, c.sd, px(c), false);
  film_write_denoising_glass_resolve(c.kg, c.state, c.sd, &c.sd->closure[0], LABEL_TRANSMIT | LABEL_SINGULAR, px(c));
  c.state->path.bounce = 1;
  set_diffuse(wall, 9.0f, make_float3(1.0f, 1.0f, 1.0f));
  film_write_denoising_features_surface(c.kg, c.state, wall, px(c), false);
  dump(c);
  CHECK(fabsf(c.buf[P_ALBEDO] - 0.9f) < 1e-3f && fabsf(c.buf[P_ALBEDO + 1] - 0.6f) < 1e-3f && fabsf(c.buf[P_ALBEDO + 2] - 0.3f) < 1e-3f,
        "albedo = (0.9 0.6 0.3), no gain above the glass colour");

  printf("== T4b glass 80%% + diffuse 20%% mixture: the mixture share is divided out ==\n");
  setup(c, true);
  set_glass(c.sd, 5.0f, 0.0f);
  c.sd->closure[0].weight = make_float3(0.8f, 0.8f, 0.8f);
  c.sd->closure[0].sample_weight = 0.8f;
  c.sd->num_closure = 2;
  ShaderClosure *dc = &c.sd->closure[1];
  dc->type = CLOSURE_BSDF_DIFFUSE_ID;
  dc->weight = make_float3(0.1f, 0.1f, 0.1f);
  dc->sample_weight = 0.2f;
  dc->N = make_float3(0, 0, -1);
  film_write_denoising_features_surface(c.kg, c.state, c.sd, px(c), false);
  CHECK(c.state->path.flag & PATH_RAY_GLASS_PENDING, "still counts as smooth glass (glass dominates the mixture)");
  film_write_denoising_glass_resolve(c.kg, c.state, c.sd, &c.sd->closure[0], LABEL_TRANSMIT | LABEL_SINGULAR, px(c));
  c.state->path.bounce = 1;
  set_diffuse(wall, 9.0f, make_float3(1.0f, 1.0f, 1.0f));
  film_write_denoising_features_surface(c.kg, c.state, wall, px(c), false);
  dump(c);
  CHECK(fabsf(c.buf[P_ALBEDO] - 1.0f) < 1e-3f, "albedo = 1.0: the glass share is not applied twice (got %.3f)", c.buf[P_ALBEDO]);

  printf("== T5 chain: the far wall of a bottle is smooth glass again ==\n");
  setup(c, true);
  set_glass(c.sd, 5.0f, 0.0f);
  film_write_denoising_features_surface(c.kg, c.state, c.sd, px(c), false);
  film_write_denoising_glass_resolve(c.kg, c.state, c.sd, &c.sd->closure[0], LABEL_TRANSMIT | LABEL_SINGULAR, px(c));
  c.state->path.bounce = 1;
  ShaderData *g2 = (ShaderData *)aligned_alloc(16, sizeof(ShaderData) + 64);
  set_glass(g2, 6.0f, 0.0f);
  film_write_denoising_features_surface(c.kg, c.state, g2, px(c), false);
  dump(c);
  CHECK(all_zero(c), "second glass writes nothing yet");
  CHECK((c.state->path.flag & PATH_RAY_GLASS_PENDING) && !(c.state->path.flag & PATH_RAY_GLASS_THROUGH), "PENDING again, THROUGH handed over");
  /* second glass REFLECTS: its own surface guides, written as for a first hit at absolute depth */
  film_write_denoising_glass_resolve(c.kg, c.state, g2, &g2->closure[0], LABEL_REFLECT | LABEL_SINGULAR, px(c));
  dump(c);
  CHECK(fabsf(c.buf[P_DEPTH] - 6.0f) < 1e-3f, "second glass depth 6 absolute (got %.3f)", c.buf[P_DEPTH]);
  CHECK(c.buf[P_SPEC_ALBEDO] > 0.0f, "second glass specular albedo written");

  printf("== T6 through, then the SKY: same far depth a primary ray would write ==\n");
  setup(c, true);
  set_glass(c.sd, 5.0f, 0.0f);
  film_write_denoising_features_surface(c.kg, c.state, c.sd, px(c), false);
  film_write_denoising_glass_resolve(c.kg, c.state, c.sd, &c.sd->closure[0], LABEL_TRANSMIT | LABEL_SINGULAR, px(c));
  c.state->path.bounce = 1;
  film_write_denoising_features_background(c.kg, c.state, px(c));
  dump(c);
  CHECK(c.buf[P_DEPTH] == FLT_MAX, "sky depth overwritten with FLT_MAX");
  CHECK(!(c.state->path.flag & PATH_RAY_GLASS_THROUGH) && (c.state->path.flag & PATH_RAY_DENOISING_FEATURES), "THROUGH cleared, features kept for the sky albedo");

  printf("== T7 rough glass is not smooth: stock path, no holding back ==\n");
  setup(c, true);
  set_glass(c.sd, 5.0f, 0.5f);
  film_write_denoising_features_surface(c.kg, c.state, c.sd, px(c), false);
  dump(c);
  CHECK(!(c.state->path.flag & PATH_RAY_GLASS_PENDING), "no PENDING for rough glass");
  CHECK(!all_zero(c), "guides written immediately");

  printf("== T8 follow_reflections on: feature is off ==\n");
  setup(c, true, true);
  set_glass(c.sd, 5.0f, 0.0f);
  film_write_denoising_features_surface(c.kg, c.state, c.sd, px(c), false);
  CHECK(!(c.state->path.flag & PATH_RAY_GLASS_PENDING), "no PENDING with follow reflections");

  printf("== T9 a diffuse first hit is untouched by the feature ==\n");
  setup(c, true);
  set_diffuse(c.sd, 5.0f, make_float3(0.5f, 0.4f, 0.3f));
  film_write_denoising_features_surface(c.kg, c.state, c.sd, px(c), false);
  dump(c);
  CHECK(!(c.state->path.flag & PATH_RAY_GLASS_PENDING) && fabsf(c.buf[P_DEPTH] - 5.0f) < 1e-3f, "written as usual");

  printf("== T10 matte only (mode 2): the glass's own guides become constant and diffuse-like ==\n");
  setup(c, false, false, true);
  set_glass(c.sd, 5.0f, 0.0f);
  film_write_denoising_features_surface(c.kg, c.state, c.sd, px(c), false);
  dump(c);
  CHECK(!(c.state->path.flag & PATH_RAY_GLASS_PENDING), "not held back (through is off)");
  CHECK(fabsf(c.buf[P_ALBEDO] - 0.5f) < 1e-3f && fabsf(c.buf[P_ALBEDO + 2] - 0.5f) < 1e-3f, "diffuse albedo constant 0.5");
  CHECK(c.buf[P_SPEC_ALBEDO] == 0.0f && c.buf[P_SPEC_ALBEDO + 1] == 0.0f, "no specular albedo");
  CHECK(fabsf(c.buf[P_ROUGH] - 1.0f) < 1e-3f, "roughness 1 (got %.3f)", c.buf[P_ROUGH]);
  CHECK(fabsf(c.buf[P_DEPTH] - 5.0f) < 1e-3f && fabsf(c.buf[P_NORMAL + 2] + 1.0f) < 1e-3f, "depth and normal are still the glass's own");
  CHECK(c.state->path.flag & PATH_RAY_GLASS_SEEN, "SEEN: no hit distance for matte glass either");
  c.state->path.bounce = 1;
  c.state->path.glossy_bounce = 1;
  film_write_denoising_specular_hit_distance(c.kg, c.state, 3.0f, px(c));
  CHECK(c.buf[P_HITDIST] == 0.0f, "no hit distance");

  printf("== T11 both (mode 3): reflecting samples write the MATTE glass, refracting ones the background ==\n");
  setup(c, true, false, true);
  set_glass(c.sd, 5.0f, 0.0f);
  film_write_denoising_features_surface(c.kg, c.state, c.sd, px(c), false);
  CHECK(all_zero(c) && (c.state->path.flag & PATH_RAY_GLASS_PENDING), "held back");
  film_write_denoising_glass_resolve(c.kg, c.state, c.sd, &c.sd->closure[0], LABEL_REFLECT | LABEL_SINGULAR, px(c));
  dump(c);
  CHECK(fabsf(c.buf[P_ALBEDO] - 0.5f) < 1e-3f && c.buf[P_SPEC_ALBEDO] == 0.0f && fabsf(c.buf[P_ROUGH] - 1.0f) < 1e-3f, "reflecting sample: matte glass guides");
  setup(c, true, false, true);
  set_glass(c.sd, 5.0f, 0.0f);
  film_write_denoising_features_surface(c.kg, c.state, c.sd, px(c), false);
  film_write_denoising_glass_resolve(c.kg, c.state, c.sd, &c.sd->closure[0], LABEL_TRANSMIT | LABEL_SINGULAR, px(c));
  c.state->path.bounce = 1;
  set_diffuse(wall, 9.0f, make_float3(0.2f, 0.3f, 0.4f));
  film_write_denoising_features_surface(c.kg, c.state, wall, px(c), false);
  dump(c);
  CHECK(fabsf(c.buf[P_ALBEDO] - 0.2f) < 1e-3f && fabsf(c.buf[P_ALBEDO + 2] - 0.4f) < 1e-3f, "refracting sample: the wall's own albedo, not the matte constant");
  CHECK(fabsf(c.buf[P_DEPTH] - 9.0f) < 1e-3f, "wall depth 9");

  printf("== T12 matte leaves a diffuse surface and rough glass alone ==\n");
  setup(c, false, false, true);
  set_diffuse(c.sd, 5.0f, make_float3(0.7f, 0.6f, 0.5f));
  film_write_denoising_features_surface(c.kg, c.state, c.sd, px(c), false);
  CHECK(fabsf(c.buf[P_ALBEDO] - 0.7f) < 1e-3f, "diffuse albedo untouched (%.3f)", c.buf[P_ALBEDO]);
  setup(c, false, false, true);
  set_glass(c.sd, 5.0f, 0.5f);
  film_write_denoising_features_surface(c.kg, c.state, c.sd, px(c), false);
  CHECK(c.buf[P_SPEC_ALBEDO] > 0.5f && !(c.state->path.flag & PATH_RAY_GLASS_SEEN), "rough glass keeps the stock guides");

  printf("== T13 matte with follow_reflections on: off ==\n");
  setup(c, false, true, true);
  set_glass(c.sd, 5.0f, 0.0f);
  film_write_denoising_features_surface(c.kg, c.state, c.sd, px(c), false);
  CHECK(c.buf[P_SPEC_ALBEDO] == 1.0f, "stock guides with following reflections");

  printf("== T14 slightly rough glass (roughness 0.07, alpha 0.0049): smooth only with a level that allows it ==\n");
  g_level = 0;
  setup(c, true);
  set_glass(c.sd, 5.0f, 0.0049f);
  film_write_denoising_features_surface(c.kg, c.state, c.sd, px(c), false);
  CHECK(!(c.state->path.flag & PATH_RAY_GLASS_PENDING) && !all_zero(c), "level 0: not smooth, stock guides");
  g_level = 1; /* roughness up to 0.05: still not */
  setup(c, true);
  set_glass(c.sd, 5.0f, 0.0049f);
  film_write_denoising_features_surface(c.kg, c.state, c.sd, px(c), false);
  CHECK(!(c.state->path.flag & PATH_RAY_GLASS_PENDING), "level 1 (<= 0.05): still not smooth");
  g_level = 2; /* roughness up to 0.1: yes */
  setup(c, true);
  set_glass(c.sd, 5.0f, 0.0049f);
  film_write_denoising_features_surface(c.kg, c.state, c.sd, px(c), false);
  CHECK(c.state->path.flag & PATH_RAY_GLASS_PENDING && all_zero(c), "level 2 (<= 0.1): smooth, held back");
  g_level = 3; /* roughness up to 0.2 */
  setup(c, false, false, true);
  set_glass(c.sd, 5.0f, 0.0049f);
  film_write_denoising_features_surface(c.kg, c.state, c.sd, px(c), false);
  CHECK(fabsf(c.buf[P_ALBEDO] - 0.5f) < 1e-3f, "level 3 with the matte mode: matte guides");
  setup(c, true);
  set_glass(c.sd, 5.0f, 0.5f); /* roughness ~0.84 */
  film_write_denoising_features_surface(c.kg, c.state, c.sd, px(c), false);
  CHECK(!(c.state->path.flag & PATH_RAY_GLASS_PENDING), "level 3: a really rough glass is still not smooth");
  g_level = 0;

  printf("\n%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
  return g_fail ? 1 : 0;
}
