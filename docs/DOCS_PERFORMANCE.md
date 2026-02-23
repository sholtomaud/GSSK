# GSSK Performance & Timestep Scaling

## The Animation Clamp Issue

In web-based simulations, developers often use `requestAnimationFrame` to synchronize simulation steps with the display's refresh rate (typically 60Hz). While this provides smooth animations, it introduces a hard bottleneck:

If a simulation requires 100,000 steps to complete (`t_end=10000, dt=0.1`) and we perform a fixed number of steps per frame (e.g., 50), the simulation is artificially capped.

- **Total Steps:** 100,000
- **Steps per Frame:** 50
- **Total Frames:** 2,000
- **Time at 60fps:** ~33.3 seconds

Even if the WebAssembly (WASM) kernel can compute those 100,000 steps in less than 100ms, the user is forced to wait 33 seconds to see the result.

## Chart Rendering Bottlenecks

As the simulation progresses, the amount of data points sent to the visualization engine (like Chart.js) grows linearly. Rendering 100,000 points on a single canvas is computationally expensive and can lead to UI unresponsiveness, regardless of how fast the numerical engine is.

## Solutions Implemented

### 1. Adaptive Step Batching (Timestep Scaling)
Instead of a fixed 50 steps per frame, the engine now calculates how many steps are needed to complete the simulation within a "Reasonable Time" (e.g., 2-5 seconds). It scales the batch size per frame dynamically.

### 2. UI-Side Data Decimation
To keep the visualization performant, we don't need to draw every single `dt` step if the dataset is massive. We implement a decimation strategy:
- **Max Points:** Limit the chart to ~2000-5000 points.
- **Sampling:** Only push data to the chart at an interval `N = total_steps / max_points`.

### 3. User-Controlled Parameters
We provide the user with direct control over `t_start`, `t_end`, and `dt`, allowing them to balance accuracy and simulation duration.

## Streaming vs. Bulk Mode

- **Streaming:** Optimized for real-time feedback and "ticker" style updates. Now uses adaptive batching to ensure it stays fast even for long durations. This mode is ideally suited for scenarios like exchange ticker timeseries or websocket streams where the simulation must project forward quickly.
- **Bulk:** Computes everything as fast as possible in a single block, but utilizes decimation before the final render to prevent browser hangs.

## The "Ticker" Analogy

Streaming simulation in GSSK is designed to handle high-frequency updates. By decoupling the numerical integration from the UI refresh rate (via adaptive batching), GSSK can process thousands of internal "ticks" while maintaining a smooth 60fps visual representation. This ensures that even as the complexity or duration of a simulation grows, the user experience remains responsive and the time-to-result stays within a reasonable bound.
