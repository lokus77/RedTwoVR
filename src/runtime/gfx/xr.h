// OpenXR (Windows, D3D12): the game's picture on a virtual screen in a VR headset.
// Off unless asked for (--xr or R2_XR=1); builds without R2_OPENXR have only the stubs.
#pragma once

namespace rt::xr {

// Before aurora_initialize: the OpenXR instance and the headset (system); tells Aurora the
// adapter the headset is on. False: no VR this run (not asked for, no runtime, no headset).
bool preinit(int argc, char** argv);
// After aurora_initialize: the session on Aurora's D3D12 device, and the per-frame hook.
void start();
// VR this run (a headset session, or R2_XR_FAKE): the VR page's resolution applies
bool enabled();
// The eyes' height the OpenXR runtime recommends (pixels; 0: no headset)
int recommendedEyeHeight();
// The session is running (the headset shows the screen).
bool active();
// Stereo 3D is on (--xr, unless R2_XR_MODE=screen): Aurora renders both eyes.
bool stereo();
// At the start of each frame (the game's thread): the eyes' views for its draws, as
// GX_AURORA_SET_STEREO_VIEWS takes them (per eye: tan left/right/down/up, then a 3x4 view).
// False: the draws take the game's own projection (no session, the menus up).
bool frameViews(float out[2][16]);
// Each game frame (hle_game.cpp): the player is flying. Stereo 3D only then; the menus, the
// hangar, cinematics and the pause screen stay on the flat screen (drawn for a 4:3 TV, their
// edges would fall outside the headset's comfortable view).
void setFlight(bool flying);
// In flight (hle_CameraSetup, the game's thread): the head's orientation now, as a rotation
// (rows; head axes -> the game camera's), which the game's camera takes so that it culls and
// lays the terrain out where the player looks; the eyes' views are then relative to it.
// False: no head (the camera stays the game's).
bool cameraRotation(float r[9]);
// In flight (hle_CameraSetup): the game camera's view as it shows it (vertical fov in degrees,
// aspect), which the HUD is laid out for
void setGameView(float fovY, float aspect);
// The VR page of the menu changed a setting: the headset's refresh rate, the HUD's distance and
// the screen's size take it at once (stereo 3D from the next start)
void applySettings();
// The VR page's "Reset camera zoom": the eyes back at the game's camera (xr_camera_forward 0)
void resetCameraZoom();
// A vibration of both Touch controllers (amplitude 0..1, seconds), e.g. as the player fires;
// any thread (the next presented frame starts it). A stronger one replaces a weaker one pending.
void haptic(float amplitude, float seconds);

}  // namespace rt::xr
