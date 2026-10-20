#pragma once

// Lightweight runtime diagnostics for devices where the app can be killed (memory) or stall without leaving a crash log.
// Logs per-second FPS, memory footprint / remaining budget, memory pressure events and, when no frame is presented for a few
// seconds, a snapshot of what every thread is doing.
void RuntimeDiagnostics_Start();
void RuntimeDiagnostics_Stop();
