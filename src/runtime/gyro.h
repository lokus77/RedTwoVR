// Gyro aiming (gyro.cpp): the controller's gyro - the Steam Deck's own, a DualSense's, a Switch
// Pro Controller's - steers the craft in flight, on top of the main stick. Settings: gyroMode / gyroButton / gyroSpeedX / gyroSpeedY /
// gyroInvertY (Controls page).
#pragma once
#include <string>

#include "runtime.h"

struct PADStatus;

namespace rt::gyro {

void init();  // with Aurora's pads: registers the input layer that collects sensor samples
// once per retrace from input::poll, after PADRead: adds the gyro to port 1's main stick.
// joy: the input thread's controller lock is held (SDL's controller API may be called)
void apply(PADStatus& port1, bool joy);
void setFlight(bool flying);  // each game frame (hle_game.cpp): the player's craft flies
bool flying();  // (the last setFlight)
// the controller whose gyro is read (empty: none found) - for the menu
std::string controllerName();

// the activation buttons offered in the menu (SDL gamepad button names; misc3 / misc4 are the
// Steam Deck's stick touches; paddle1-4 its back buttons R4 / L4 / R5 / L5, an Xbox Elite's
// P1 / P3 / P2 / P4; "touch" is not an SDL button: a finger on the touchpad - DualSense, DualShock 4)
struct Button {
    const char* sdlName;
    const char* label;
};
inline constexpr Button kButtons[] = {
    {"misc3", "Left stick touch"},    {"misc4", "Right stick touch"},  {"touch", "Touchpad touch"},
    {"touchpad", "Touchpad click"},   {"paddle1", "R4 / P1 paddle"},   {"paddle2", "L4 / P3 paddle"},
    {"paddle3", "R5 / P2 paddle"},    {"paddle4", "L5 / P4 paddle"},   {"rightshoulder", "RB / R1"},
    {"leftshoulder", "LB / L1"},      {"rightstick", "Right stick click"}, {"leftstick", "Left stick click"},
    {"misc1", "Mic / Capture / Share"},
};
inline constexpr int kButtonCount = int(sizeof kButtons / sizeof kButtons[0]);
// which of kButtons the gyro's controller has (bit i; all of them while there is none), and the
// one used: the setting's, or when that controller lacks it the first of left stick touch,
// touchpad touch and left stick click it has (an index into kButtons)
u32 availableButtons();
int effectiveButton();

}  // namespace rt::gyro
