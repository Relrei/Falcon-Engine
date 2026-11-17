# Falcon Engine V0.2.0 — Features and usage

> Names below are the labels shown in Blender. Items marked *(known issue)* are open.

Falcon Engine is a custom build of Blender 5.3 (based on Blender, not affiliated with the Blender Foundation).
It replaces Cycles with the render engine **Cycles-expansion** (Cycles with Falcon's additions; with Bidirectional off and Mode set to Expansion it works like Cycles). EEVEE is unchanged. The official Cycles can be built next to it with `WITH_FALCON_OFFICIAL_CYCLES=ON` (see BUILD.md).

## 1. Turning it on

1. Properties → Render → Render Engine: **Cycles-expansion**.
2. Edit → Preferences → Add-ons → Cycles-expansion: pick **OptiX** (recommended) or CUDA and tick your GPU.
3. Render → Device: **GPU Compute**.
4. Render → **Mode**:

| Mode | What it does |
|---|---|
| **Expansion** (default) | Cycles with Falcon's additions (bidirectional light transport and more). Sampling and denoising work as in Cycles. |
| **Neural Rendering** | Built on DLSS Ray Reconstruction. Few samples per frame, extra samples on glass, and the DLSS history carries the image. Render and viewport always denoise with DLSS. Needs an NVIDIA RTX GPU and the DLSS runtime (see 6). |

## 2. Caustics and bidirectional light transport

Render → Light Paths → **Bidirectional** (tick the header to turn it on).

| Setting | Meaning |
|---|---|
| **Automatic** (default on) | Falcon picks the techniques for the scene, and after the first sample stops the ones that add next to nothing, so scenes without caustics do not get slower. Scenes lit only by emissive meshes stop it at once. |
| **Photons** | Light paths of the photon pass at the start of each frame (caustics seen through glass or water). More = smoother and sharper, 48 bytes each. Default 1,000,000. |
| **Caustic Strength** | Multiplier on the caustics from light tracing and photon mapping. 1 is physically correct. |

Untick **Automatic** to choose by hand:

| Strategy | Meaning |
|---|---|
| **Light Tracing** | Paths start at the lights and connect to the camera and to the camera paths. Gives caustics on diffuse surfaces (a glass's bright spot on the table) and light shafts through fog. |
| **Photon Mapping** | Photons from the lights are gathered on the camera paths. For caustics seen *through* glass or water (pool floor, caustics inside a bottle). |
| **Manifold Walks** | Not implemented yet: shown, but renders without it. |
| **Light Bounces** | Maximum bounces of the light paths (default 8). |

Notes:
- Light paths also pass through fog (volumes): light shafts from a spot light or through a prism show up in the fog.
  Heterogeneous fog is stepped (Volumes → Steps, default 32). *(known issue: fog is about 25 % darker than a reference renderer)*
- Bidirectional uses one GPU.
- The viewport renders without bidirectional; use a final render (F12) to see it.

## 3. Neural Rendering — final render

Render → Sampling → Render (in Neural Rendering mode):

| Setting | Meaning |
|---|---|
| **Samples** | Samples of every pixel (no noise threshold). |
| **Glass** | Samples of the pixels whose camera ray first hits glass. Not above Samples turns it off. Not used with an upscaling DLSS. |
| **Halve Samples Each Frame** | Animations: frames after the first (and after each cut) lean on the DLSS history; pixels without glass get half the samples of the frame before, down to 1 (8, 4, 2, 1 …). Glass pixels keep Glass Samples. |
| **Time Limit** | As in Cycles. |

**Glass Through Guides** (default **Albedo and Normal**): tells DLSS what is seen through glass, so the
surface behind glass stays sharp instead of smeared. Off = the guides as Cycles writes them.

Bidirectional works together with Neural Rendering (caustics are passed into DLSS).
*(known issue: a cyan halo can appear around a dispersion rainbow on a white wall in Neural Rendering only)*

## 4. Neural Rendering — viewport

Render → Sampling → Viewport (in Neural Rendering mode):

| Setting | Meaning |
|---|---|
| **Samples per Frame** | Samples per pixel of each viewport frame (default 3). While the view is still, frames go on up to Max Samples. |
| **While Moving** | Samples per frame while the view moves. |
| **Glass** | Samples of glass pixels per frame. Not used with History off nor with an upscaling DLSS. |
| **Max Samples** | Where the still view stops. |
| **History → Carry Over Frames** | DLSS carries its history from frame to frame. Off clears it each frame: no ghosting after a move, but more grain or flicker, and Glass is not used. |

Render → Light Paths → **Neural Rendering Viewport**: with **Scene Light Paths** off (default) the viewport
uses bounces chosen by Falcon (shown in the panel); the final render always uses the scene's Light Paths.

## 5. Other additions

| Setting | Where | Meaning |
|---|---|---|
| **Unlimited** | Sampling → Render (Expansion mode) | Final renders keep adding samples until stopped (Esc keeps the image). Only without denoising. |
| **Light Mixer** | View Layer → Light Mixer (uses the Light Groups) | Change the strength and color of each light group after rendering, in the compositor, without rendering again. |
| **Shadow Caustics** | Light, and Object → Shadow Caustics | As in Cycles: approximate caustics in the shadows of refractive objects (light, caster and receiver must be set). |

## 6. DLSS runtime

DLSS Ray Reconstruction is made by NVIDIA and is not part of Falcon Engine.
Falcon Engine loads it at run time; building needs only the headers of the DLSS SDK (see BUILD.md).
- Put the runtime (`libnvidia-ngx-dlssd.so.*` on Linux) in a folder and set
  Preferences → Add-ons → Cycles-expansion → **DLSS Runtime Folder**, then restart Blender.

## 7. Requirements (decided 2026-11-16)

- NVIDIA GeForce RTX 20 series or newer, driver 580 or newer. Tested on RTX 30 / 40.
- Linux first; other distributions supported, Windows later.
- Without an NVIDIA GPU, Cycles-expansion can still render on the CPU, and EEVEE works as usual. AMD (HIP) and Intel (oneAPI) GPU rendering are not built by default and not tested.
