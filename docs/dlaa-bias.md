# Experimental DLAA animated-object history bias

The Windows DLSS bridge supports NVIDIA's native-resolution DLAA. The game requests
**Preset K** for DLAA by default; `BB_DLAA_PRESET` can switch the request for
controlled comparisons. DLAA uses the existing camera and G-buffer object motion vectors
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

- `BB_DLAA_PRESET=K` (default): explicitly request transformer Preset K (11)
  for DLAA. Use `BB_DLAA_PRESET=J` to request Preset J (10), which NVIDIA
  describes as potentially less ghosting at the cost of more flicker, or
  `BB_DLAA_PRESET=default` to let NGX choose. Other DLSS quality modes still
  request the driver default. Restart the game after changing this setting;
  the log reports the *requested* preset, not proof of which model was loaded.
- `BB_DLAA_BIAS=0` (default): skip the legacy current-color bias mask,
  allocation and dispatch. Newer DLAA models (including Preset K) do not
  support this input.
- `BB_DLAA_BIAS=1` (experimental opt-in): submit the mask for comparison
  with older models. Submitting it does not mean Preset K uses it.
- `BB_DLAA_BIAS_STRENGTH=1.0` (default): multiply the mask by this factor.
  The accepted range is `0.0`–`2.0`; invalid values fall back to `1.0`.
  Increase it if trails remain, or reduce it if textures flicker or soften.
  The shader clamps the final mask to `[0,1]`. `0.0` skips the bias dispatch
  and avoids allocating its render-size texture, saving GPU work and memory.
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
`BB_PROBE`/launcher options. The run log should show `DLSS: ready` and `preset 11`. The
`DLAA: animated-object history bias enabled` message now appears only with
explicit `BB_DLAA_BIAS=1`.

For a controlled comparison, use the same scene and settings with
`BB_DLAA_BIAS=0` and `1`. Record both ghosting and frame time, especially
at high output resolutions. The Vulkan shader is built by `bash build.sh`,
not by the bridge script.

## Motion-vector correctness experiments

The character's G-buffer skeleton draw already stores and reuses clip-space
vertex positions. It supplies pixel-space current-to-previous motion to DLAA;
the camera motion shader fills in other scene pixels.

The following changes are on the DLAA test branch:

- **Unreadable small bone palettes:** when a selected small-skeleton vertex
  shader's bone palette cannot be accessed for hashing, the rasterizer now
  records object positions instead of assuming a constant palette means the
  weapon/prop is motionless. This is a correctness fix, but has not been
  benchmarked or visually validated on an RTX 3080.
- **Behind-previous-camera reprojection:** `camera_motion.comp` rejects motion
  derived from geometry behind the previous camera or non-finite/extreme
  reprojections. It sends a finite, off-screen vector instead of overflowing
  the R16G16 motion texture. This should prevent invalid temporal reads during
  large camera changes. In-game validation is still required.
- **`BB_OBJECT_MOTION_SMALL=1` (opt-in):** bypass the static-palette gate on
  selected small-skeleton geometry. It can help rigidly animated weapons,
  accessories, and props whose model transform moves without changing the bone
  palette. It may increase vertex work and VRAM bandwidth: compare frame rate
  and ghosting before making it a default. `BB_OBJECT_MOTION_ALL=1` is broader
  and may be substantially more expensive.
- **`BB_MOTION_INDEX_STRICT=1` (opt-in):** verify the indexed geometry's
  range/topology at least once per frame, instead of reusing it for up to
  32 frames. Prevents incorrect previous-vertex matches if a buffer is rewritten
  with new indices at the same address; it adds CPU hashing/scanning overhead.
  Test this separately from the small-skeleton option. Most skinned meshes
  use stable index buffers and should not need it.
- **Small-skeleton animation stop transitions:** when bone movement ends, the
  history gate now tracks *one final unchanged frame* instead of shutting off
  immediately. This lets the final frame of a sword swing/weapon animation
  reproject using the last moving frame. Static meshes don't incur continuing
  vertex-history cost. This is on by default.
- **Faster motion composition:** `camera_motion.comp` uses valid skinned-object
  vectors directly, avoiding full camera reprojection for those pixels. Object
  motion is rejected when the final scene depth no longer agrees, or when its
  magnitude is too large to represent safely in the RG16F DLAA motion texture;
  camera reprojection is used for those pixels. Applies to DLAA, FSR and TAA.
- **`BB_MOTION_DEPTH_TOLERANCE=0.003` (opt-in):** tune how strictly object-motion
  depth must agree with the final depth buffer. The default is `0.01`, preserving
  previous behavior. Lower values (e.g. `0.003`) can reject more stale foreground
  vectors at silhouettes, but can also reject correct vectors from depth rounding,
  increasing blur. The accepted range is `0.0` to `0.05`; invalid values revert
  to `0.01`. Test it as a separate startup-only A/B setting.

`BB_OBJECT_MOTION_SMALL` and `BB_MOTION_INDEX_STRICT` both default to off.
They only matter with `BB_OBJECT_MOTION=1`.

## DLAA frame-rate optimization (experimental)

- **Default GPU optimization:** `camera_motion.comp` now evaluates previous-view
  reprojection using current-view *reciprocal depth*. The renderer divides the
  camera's reprojection XY coefficients by projection scale **once per frame**
  and supplies `1 / proj.w` as a push constant. This removes the view-Z
  reconstruction division (and associated multiplies) for camera-motion
  pixels. The algebra preserves previous-minus-current vectors, sky rotation,
  depth clipping and jitter cancellation. The stand-alone `motion-history-test`
  checks the old/new perspective divide for non-unit FOV, near/far scene depth
  and sky; `camera-motion-test` uses production SPIR-V on Vulkan hardware.
  **FPS gains are not yet measured** and may be small on a GPU-bound system.
- **`BB_OBJECT_MOTION_SMALL=0`**: recommended for performance. Keep normal
  bone-palette motion gating, which still tracks the character's full skeleton
  and selected moving weapon/prop palettes. Using `=1` records positions for
  every recognized small-skeleton draw, including approximately 1,000 draws
  in scenes where most are stationary. The original research in
  `docs/motion_vectors.md` found that indiscriminate tracking can overflow
  history and reduce FPS severely.
- **Do not hash 416-byte model constants to detect rigid motion by default.**
  They change with the camera on almost all draws; prior experiments slowed
  scenes to about 40 FPS. `BB_OBJECT_MOTION_SMALL=1` remains an opt-in
  correctness investigation only.
- **`BB_MOTION_INDEX_STRICT=0`**: leave the normal 32-frame index cache on
  unless a repeatable topology-history bug is diagnosed; strict mode hashes
  index buffers each frame.
- **`BB_DLAA_BIAS=0` (now default) with Preset K:** avoids the unsupported
  bias-mask texture and compute dispatch.
- **Sharpening strength at most 1.0:** values above 1.0 invoke a separate
  extra-sharpen blit and compute pass, with an RGBA32F scratch texture.
  Keep the default/no extra pass for a baseline frame-time measurement.

For A/B tests, record the same static scene, camera pan and attack at a
fixed resolution with `BB_FRAME_STATS=1`. Compare average FPS and frame-time
spikes after shaders are warm. Run once with `BB_OBJECT_MOTION_SMALL=0` and
once with `=1` to isolate the expensive override. Avoid changing DLAA
preset, resolution and model selection at the same time.

### Suggested test order

1. Keep `BB_DLAA_PRESET=K BB_DLAA_BIAS=0 BB_OBJECT_MOTION=1 BB_JITTER=1`
   with both new experimental flags off. Record a short repeatable walking,
   attack and camera-pan test at native resolution and its approximate FPS.
2. Repeat with **only** `BB_OBJECT_MOTION_SMALL=1`. Look for weapon, cape and
   accessory trails, and note any performance impact.
3. Repeat with **only** `BB_MOTION_DEPTH_TOLERANCE=0.003` and compare
   trails on high-contrast character edges; if details blur more, return to
   the `0.01` default.
4. Repeat with **only** `BB_MOTION_INDEX_STRICT=1`. If it helps, inspect
   `BB_FRAME_STATS=1` object-motion statistics and investigate index-buffer
   write frequency before enabling it by default.

`BB_MOTION_SELECT_LOG=1` can identify which G-buffer vertex shaders are
instrumented; it cannot guarantee every visible character layer has correct
motion. The game's debug motion view is useful for checking the actual vectors.
The main character's full skeleton shaders do not use small-skeleton gating,
so the opt-in option may primarily affect accessories rather than the torso.

After building, run the CPU regression test for index-history and the
one-frame animation-stop gate:

```bash
ninja -C out/gpu motion-history-test
./out/gpu/motion-history-test.exe
```

`bash build.sh` recompiles `camera_motion.comp` as a host shader. These
changes do not alter the DLSS DLL ABI, so an already-working bridge can be reused.

## Important SDK caveat and A/B tests

NVIDIA's 2026 DLSS Programming Guide (section 3.15, "Biasing the Current
Frame") says the optional bias-current-color buffer is **not supported by the
newer models** and should not be used; it notes legacy Preset F support.
Therefore the mask being submitted to NGX with Preset K does **not** prove it is
being used by that model. Any perceived improvement may be caused by the preset,
motion inputs, scene variation, or the driver. Retain the experimental path for
comparisons, but do not claim that Preset K officially supports it.

Run the same area and character animation four ways: K with bias enabled,
K with bias disabled, J with bias enabled, and J with bias disabled. Use
identical resolutions, jitter, object motion and graphics settings, and compare
both ghosting and texture flicker. Test the driver default separately if helpful.
Because the settings are read at startup, restart between runs.

Switching between FSR 4 and DLAA in the game menu also rebuilds the
DLAA-specific bias image if needed; it is not limited to the first startup mode.

## Limitations

This is an experimental, region-based hint, not a confirmed universal ghosting
fix. The mask currently samples a 3×3 neighborhood; fast disocclusions farther
than one pixel, untracked animated meshes, transparencies, particles and
mismatched depth/color may still ghost. Avoid merging without in-game tests
on the target hardware. The `windows` and CMAA2 branches are unaffected.
