#pragma once
#include <string>

#include "../ctx.h"

namespace rt {

struct PadState {
    bool connected = false;
    u16 buttons = 0x0080;  // PAD_USE_ORIGIN is always set by the controller
    u8 stickX = 0x80, stickY = 0x80, substickX = 0x80, substickY = 0x80, triggerL = 0, triggerR = 0;
};
PadState& padState(int port);

// Gather pipe (0xCC008000) -> GX command stream
void gxFifoWrite(u32 v, int size);
struct GxStats {
    u64 bytes = 0, draws = 0, vertices = 0, bp = 0, xf = 0, cp = 0, dl = 0, unknown = 0;
};
GxStats& gxStats();
// Receives every complete GX command (raw bytes, big-endian), e.g. for a renderer.
using GxSink = void (*)(const u8* cmd, u32 len);
void gxSetSink(GxSink sink);
void gxSetSyncHook(void (*hook)());  // called when the CPU reads the GP's status or pointers

// FIFO registers: CP (0xCC000000) and the PI's CPU FIFO (0xCC00300C-0x14)
void gxInit();
u16 gxCpRead(u32 off);
void gxCpWrite(u32 off, u16 v);
u32 gxPiRead(u32 off);
void gxPiWrite(u32 off, u32 v);
void gxResetGatherPipe();  // WPAR write
void gxFlushGatherPipe();  // PPCSync: drain a partly filled gather buffer
std::string gxFifoText();  // the FIFOs' pointers and breakpoint, for stall reports

// PE events raised by the stream
void peToken(u16 token, bool irq);
void peFinish();

}  // namespace rt
