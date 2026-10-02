#pragma once

// TEMPORARY diagnostic experiment — remove.
//
// Toggle 0 = normal, 1 = freeze. In freeze mode the GUI task renders and
// presents exactly ONE frame, then never calls gfx_present() again for the
// rest of the run. The panel is left free-running on whatever it last
// received from us.
//
// The point: if the glass still shows spinning/tearing with the framebuffer
// completely untouched, then no code in this firmware is producing it and the
// fault is at or below the panel driver.

#define FREEZE_AFTER_ONE_FRAME 1