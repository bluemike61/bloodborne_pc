# CMAA2 Vulkan implementation (experimental)

This patch adds Intel CMAA2 as a separate **native-resolution spatial AA** mode (`BB_UPSCALER=cmaa2` or the graphics menu). It runs before UI composition. It does not use temporal history, camera jitter or FSR reconstruction, and it does not replace TAA or DLSS.

## Build

Install a DXC build with SPIR-V support (`dxc` on `PATH`) **before running CMake**. The build uses `tools/build_cmaa2_spirv.py` to retrieve the open-source Intel [CMAA2 reference](https://github.com/GameTechDev/CMAA2) and compile four compute passes for HDR and LDR. You can inspect the upstream license before downloading. An optional `--source` argument accepts an offline local `CMAA2.hlsl`. The exact source used to compile is saved as `CMAA2.bbport.hlsl` in the generated build-header directory.

If DXC is absent, the normal game still builds and the CMAA2 entry is marked unsupported. **Do not select CMAA2 without building the shaders.** Switching modes is supported. On unsupported formats/sizes this experimental implementation disables CMAA2 instead of pretending to perform anti-aliasing.

## Implementation

- HDR (usually 1080p): dispatch Intel CMAA2 on RGBA16F scene color, then the game's original post-processing and HUD. Alpha from the source scene is preserved for later post-processing.
- Scaled-display host path (other resolutions): when the scene is already at output resolution and the LDR texture is RGBA8 UNORM, dispatch to the native-resolution `ui_image` before HUD draws.
- Scratch images: packed R8_UINT edges and R32_UINT blend list heads. Scratch buffers: full-capacity candidate list, deferred blend location list, deferred blend item list, 64-byte control counters and 16-byte Vulkan indirect-dispatch arguments.
- Shader order: EdgesColor2x2CS, ComputeDispatchArgsCS (2x1), ProcessCandidatesCS (indirect), ComputeDispatchArgsCS (1x2), DeferredColorApply2x2CS (indirect). Vulkan compute/indirect transfer barriers are recorded between steps.

## Verification / limitations

**This integration is experimental and has not been built with DXC or tested in Bloodborne in the authoring environment.** Test with a Vulkan validation layer at 1080p and 1440p/4K, especially after changing output size. Confirm background and UI alpha, scene colors, fast camera movement and menu transitions. The HLSL source is retrieved at build time, not embedded in the patch; builds must have internet access (or the script should be given a locally downloaded source). Frames with unsupported formats currently fall back to the original renderer after reporting a failure; no alternate CMAA algorithm is silently used.

The fork's original authors and the Intel CMAA2 project retain their respective copyright and license terms.
