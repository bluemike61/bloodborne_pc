// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Convert a previous-view * current-inverse-view 3x4 affine transform into
// coefficients for camera_motion.comp's depth-reciprocal reconstruction.
//
// Instead of reconstructing z and x/y in view space at *every pixel*, evaluate
// the previous view divided by current z.  For a non-sky pixel:
//     1/z = (depth - proj.z) / proj.w
//     previous_view / z = normalized_reproject * vec4(ndc.xy, 1, 1/z)
// For sky, the last component is zero and there is no camera translation.
// The perspective division cancels the common factor z.
#include <array>

namespace Vulkan::Motion {

inline std::array<float, 12> NormalizeReprojection(const std::array<float, 12>& reprojection,
                                                    const std::array<float, 4>& projection) {
    auto result = reprojection;
    for (int row = 0; row < 3; ++row) {
        result[row * 4] /= projection[0];
        result[row * 4 + 1] /= projection[1];
    }
    return result;
}

} // namespace Vulkan::Motion
