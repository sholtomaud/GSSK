# GPU Acceleration in GSSK

This document addresses the feasibility of using GPU acceleration for intensive simulations in GSSK, both in WebAssembly (WASM) and Native CLI environments.

## WebAssembly (WASM) and WebGPU

### Feasibility
Yes, WASM code can utilize **WebGPU** for intensive simulations. WebGPU is the successor to WebGL, providing a more modern and performant interface to the GPU, specifically designed for compute tasks as well as graphics.

### Implementation Path
1.  **Emscripten Integration**: Emscripten provides headers (`emscripten/webgpu.h`) and libraries to interface with the browser's WebGPU API from C/C++.
2.  **Compute Shaders**: The core simulation loop (currently in `GSSK_Step` in `src/gssk.c`) would need to be ported to **WGSL (WebGPU Shading Language)**.
3.  **Parallelization**: ODE solving for multiple nodes or large networks can be parallelized. Each workgroup in a compute shader could handle a subset of nodes or edges.
4.  **Data Transfer**: State vectors would be stored in GPU buffers, minimizing the need to copy data back to the CPU between steps.

## Native CLI and System GPU

### Feasibility
The native CLI environment has direct access to standard system GPU APIs, making it highly suitable for intensive simulations.

### Implementation Path
1.  **APIs**:
    -   **CUDA**: Best for NVIDIA GPUs, offering high performance and mature tooling.
    -   **OpenCL**: A cross-platform standard that works on most GPUs (NVIDIA, AMD, Intel).
    -   **Vulkan**: Provides low-level GPU access with high portability, including a robust compute pipeline.
2.  **Hybrid Model**: The current C engine can be extended to support "Hardware Abstraction Layers" (HALs). For large models, the kernel could offload `compute_derivatives` to the GPU.

## Current Status
The current implementation of GSSK is a **CPU-based, sequential engine** optimized for accuracy and ease of use. While GPU acceleration is not yet implemented, the architecture (opaque instances and clear separation of state/logic) is well-suited for a future parallelized backend.
