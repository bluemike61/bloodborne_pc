# Experimental DLAA animated-object history bias

The Windows DLSS bridge supports NVIDIA's native-resolution DLAA. The game requests
**Preset K** for DLAA. DLAA uses the existing camera and G-buffer object motion vectors
and the existing jitter sequence. Disabling either can make character trails worse.

## Character ghosting mitigation

When DLAA is active at native resolution and object motion was captured for the
current frame, `dlss_bias.comp` builds a render-size R8 current-color bias image.
The mask is strongest at fast-moving character pixels and covers a one-pixel
outline to reduce lingering history around silhouettes. Its depth rejection
uses the same current camera projection as `camera_motion.comp`.

The rebuilt `bbport_dlss.dll` (bridge ABI 3) supplies this optional image through
`NVSDK_NGX_VK_DLSS_Eval_Params::pInBiasCurrentColorMask`. DLSS treats the hint as
a preference for current-frame color; it is **not** a substitute for correct
motion vectors or coverage of animation that the object-motion pass cannot track.
The mask is omitted when object vectors are unavailable. FSR and TAA do not use it.

## Settings (process startup)

- `BB_DLAA_BIAS=1` (default): enable the current-color mask where object
  motion is available.
- `BB_DLAA_BIAS=0`: turn off the entire mask pass for an A/B comparison.
- `BB_DLAA_BIAS_STRENGTH=1.0` (default): multiply the mask by this factor.
  The accepted range is `0.0`–`2.0`; invalid values fall back to `1.0`.
  Increase it if trails remain, or reduce it if textures flicker or soften.
  The shader clamps the final mask to `[0,1]`.
- `BB_OBJECT_MOTION=1` and `BB_JITTER=1`: retain geometry motion and
  camera jitter; these are important inputs rather than fixes to disable.

The strength value is read once when the first bias dispatch happens; restart
the game after changing it.

## Build and verification (MSYS2 CLANG64)

```bash
cd /e/cmaabb/bloodborne_pc
git switch feature/dlaa-ghosting-fix
git pull --ff-only
bash build.sh
# Needed when upgrading from an older bridge; requires Visual Studio 2022 Build Tools:
DLSS_SDK_ROOT=/e/cmaabb/NVIDIA-DLSS bash packaging/windows/build_dlss.sh
```

Keep `out/bbport_dlss.dll` and `out/nvngx_dlss.dll` alongside
`out/bb-probe.exe`. To request native DLAA, launch with
`BB_UPSCALER=dlss BB_UPSCALE_PRESET=0 BB_FRAME_GEN=0` and the normal
`BB_PROBE`/launcher options. The run log should show `DLSS: ready`,
`preset 11`, and the `DLAA: animated-object history bias enabled` message.

For a controlled comparison, use the same scene and settings with
`BB_DLAA_BIAS=0` and `1`. Record both ghosting and frame time, especially
at high output resolutions. The Vulkan shader is built by `bash build.sh`,
not by the bridge script.

## Limitations

This is an experimental, region-based hint, not a confirmed universal ghosting
fix. The mask currently samples a 3×3 neighborhood; fast disocclusions farther
than one pixel, untracked animated meshes, transparencies, particles and
mismatched depth/color may still ghost. Avoid merging without in-game tests
on the target hardware. The `windows` and CMAA2 branches are unaffected.
