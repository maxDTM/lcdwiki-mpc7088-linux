# Proposal for aquamarine: generic support for render-less KMS devices

Notes for a possible issue or PR against [hyprwm/aquamarine](https://github.com/hyprwm/aquamarine). **Not filed yet.** Kept here for later review. Once aquamarine handles this, `aq_evdi_compat` can be removed from this driver.

## Problem

USB display adapters handled by DRM drivers such as `ms912x` (this repo), `gud`, `udl` and `gm12u320` are KMS-only. They have no GPU and no render node, and scan out CPU-accessible (shmem or dumb) buffers. Hyprland can't drive them:

```
drm: Initializing secondary renderer on /dev/dri/card0, has enabled outputs
[EGL] Command eglQueryDeviceStringEXT errored out with EGL_BAD_PARAMETER
CDRMRenderer(drm): Can't create renderer, no matching devices found
drm: initMgpu: no renderer
drm: Failed to update renderer state for /dev/dri/card0
```

The connector is detected, but the output never lights. Observed with aquamarine 0.16.0 and Hyprland 0.57.2 on a hot-plugged device. Startup was not tested; see the open question below.

## Cause (aquamarine 0.16.0, `src/backend/drm/DRM.cpp`)

- `CDRMBackend` sets `rendererRequired = false` **only** when the DRM driver name is `evdi`:
  ```cpp
  if (driver == AQ_BACKEND_GPU_DRIVER_EVDI) {
      // DisplayLink/evdi exposes KMS without a usable EGL renderer. Keep primary so its output swapchain stays LINEAR.
      rendererRequired = false;
  }
  ```
- Every other secondary device goes through `initMgpu()` → `CDRMRenderer::attempt(gpu->renderNodeFd >= 0 ? gpu->renderNodeFd : gpu->fd)`. For a device with no EGL device this fails, and the output is never committed.
- Other places are also keyed on `AQ_BACKEND_GPU_DRIVER_EVDI` (around lines 2472, 2584, 2717 and 3151 in 0.16.0): deferring to sync commits when the KMS import fails, allowing KMS-unimportable buffers, the dumb-buffer **CPU-copy fallback**, and the cursor error code. All of these are exactly what a generic display-only device needs.

## Proof that the evdi path works for other drivers

This driver has an opt-in parameter, `aq_evdi_compat=1`, that makes it report the DRM driver name `evdi`. With it, the same aquamarine/Hyprland build:
- adopts the device on hotplug (`drm: Starting backend for /dev/dri/card0, with driver evdi with primary /dev/dri/card1`);
- creates a LINEAR multi-GPU swapchain on the primary GPU (i915), and the ms912x driver imports those dma-bufs directly, with no CPU-copy fallback needed;
- works across hotplug, a USB reset that re-probes the device, DPMS off/on and suspend/resume.

## Proposed change

Replace the driver-name check with a capability-based **display-only** flag, set when either:
- the driver is `evdi` (keeps today's behavior), **or**
- `CDRMRenderer::attempt()` fails for a **secondary** GPU, falling back instead of giving up.

A static allow-list of known USB display drivers (`gud`, `udl`, `gm12u320`, `ms912x`, `appletbdrm`, ...) would also work, but is less general. Probing for a render node isn't reliable: per an audit of the code, at startup `Session.cpp` hands node-less cards the system's only render node, while on hotplug `renderNodeFd` is `-1` even for real GPUs.

Then key the five `AQ_BACKEND_GPU_DRIVER_EVDI` checks on that flag instead of the driver name.

## Open questions before filing

- **Startup vs hotplug.** An audit of the source suggests that at startup `Session::scanGPUs` / `resolveMatchingRenderNode` give a node-less card the only render node on the system (i915's `renderD128` here), so `initMgpu` might succeed at startup. Whether it then actually works was **not tested**: the device never appeared before the compositor in any test. Test this before filing.
- Check whether wlroots, KWin and Mutter have the same limitation, for context.
- Search existing aquamarine and Hyprland issues for `gud`, `udl` or "USB display" reports first.
