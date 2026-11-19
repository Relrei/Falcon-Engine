# Falcon Engine v0.7.1

[日本語](../../README.md) | English

I started building this on my own about half a year ago, around May 2026, after a render of a car scene (a GTR) took 40 hours. I wanted to find some way to make rendering faster.
Development focuses on Linux for now. Windows will follow step by step (some features will be NVIDIA only), and macOS support is planned too, but that is still a long way off…
I had not planned to release it at first, but when DLSS Ray Reconstruction came to Blender, I decided to release it as open source.
(Honestly, I wanted to try selling it… but it would be hard to support. I use an RTX 3080, and this custom build gets the most out of RTX 30 series cards and newer, so I wanted to share it. RTX 20 series cards should work too on the technical side. GTX cards are not supported, and Radeon is not supported yet either.)

## Overview

This custom build is meant to be started through a launcher. It also starts without one, of course, but the launcher has some features of its own, and I made it because I think using the two together makes things more comfortable.

## What Falcon Engine has now

- A path to render with the DLSS Ray Reconstruction denoiser (it cannot be bundled because of its license, so you need to download it once yourself)
- Several features of the demo versions have been taken out for now; I plan to improve them and bring them back (VSE)
- Bidirectional light transport built on Cycles, so photon mapping can be used

As said above, I develop on Linux, so Linux inevitably comes first. I will work on Windows and macOS step by step (I am short on storage, so it is slow going).

## Planned

Planned additions:

- VSE
- Several new render engines
- GPU-computed simulations
- Optimizing the existing structure
- Faster sculpting
- Clusters with virtual geometry

There is much more, but these are the parts that are more or less decided at a rough level.
Please understand that development may stop suddenly for financial reasons.

Add-ons that I make myself will also become installable through the launcher:

- Real camera play
- Render scheduling through the launcher
- Data export to the Godot engine (undecided)

## Relation to the official Blender, and reporting bugs

Rather than going off on its own route, this custom build keeps the official Blender as its base while laying its own track alongside it. Compatibility problems may come up. If they do, please report them in [Issues](../../../../issues), not to the official Blender. This custom build has nothing to do with the official Blender; it is a personal custom build.
Some data also has to be downloaded and put in place by you, because of its license. Thank you for your understanding.

## Building and usage

- Launcher (Falcon Launcher): https://github.com/Relrei/Falcon-Launcher
- How to build (what you need, and the NVIDIA pages to get it from): [docs/BUILD.md](docs/BUILD.md)
- Features and usage: [docs/USAGE.md](docs/USAGE.md)
- README of the original Blender: [README.blender.md](../../README.blender.md)

No prebuilt binaries are distributed. You compile it on your own PC.
CUDA, OptiX and DLSS are made by NVIDIA and are not in this repository. Get them yourself from NVIDIA's pages, agreeing to their licenses.

NVIDIA, DLSS, OptiX and CUDA are trademarks of NVIDIA Corporation. Blender is a trademark of the Blender Foundation.
This build is not affiliated with, nor endorsed by, NVIDIA or the Blender Foundation.
