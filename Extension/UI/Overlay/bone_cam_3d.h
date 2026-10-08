#pragma once
#include "overlay.h"
#include "imgui.h"
#include <array>

// The Bone Cam's 3D X-ray (bone_cam_3d.cpp): real bone shapes built around the skater's joints
// each frame (shafts with knobbed ends, paired forearm and shin bones, vertebrae, ribs, pelvis,
// skull and jaw), shaded and depth-sorted, inside a faint shell of flesh.
namespace dingosdk::overlay::detail {
struct XrayView {
    std::array<float, 3> right{}, up{}, back{}, origin{}; // the camera's world axes and position
    float focal{};                                         // pixels per unit at depth 1
    ImVec2 centre{};
};
void draw_xray_skeleton(ImDrawList *draw, const BoneCamPose &pose, const XrayView &view, float fade);
} // namespace dingosdk::overlay::detail
