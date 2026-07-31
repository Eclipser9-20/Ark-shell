#pragma once

class JobTable;

namespace jobspanel {

// Draw the interactive jobs overlay (Ctrl+Shift+J) and handle navigation +
// actions (fg/bg/stop/kill) until the user closes it. Assumes the terminal is
// already in raw mode (the caller is readLine's key loop). Repaints/clears its
// own region on exit; the caller redraws the prompt afterward.
void show(JobTable& jobs);

}
