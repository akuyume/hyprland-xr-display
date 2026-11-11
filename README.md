# XR Hyprland

A native Wayland XR client for the original XREAL Air on Hyprland. It renders a stereo view with separate eye origins and lets you capture applications as individual panels with their own angle, depth, and size. Headless Hyprland outputs keep the applications running; XRLinuxDriver supplies head orientation. The stereo projection and tracker smoothing follow the local Air Lab reference.

**Status:** GPU capture and presentation work at 1080p/60 on an RTX 3090 with Hyprland 0.56.2. The installed Air Lab tracker passed its timestamp and factory-calibration regressions. A 35-second desk measurement showed +0.0302° yaw change. The installed renderer completed a 45-second tracked run with 2,697 frames and zero capture failures. Moving a test application into XR, recentering, stopping with application survival, and restarting passed in the live session. The wearer confirmed that enabled roll compensation keeps the window level with the room. The wearer confirmed one fused stereo panel with depth. A live stereo preview also verified two independently updating application captures in both eyes. Visible-workspace transfer preserved tiled and floating geometry in a live test, and a native 2560×1440 capture continued updating with all regular displays asleep. Physical hotplug recovery, direct spatial pointer interaction, 120 Hz, and extended performance remain open.

## Build and run

Install a C++20 compiler, CMake, pkg-config, nlohmann-json, Wayland development tools (including wayland-scanner), EGL, OpenGL ES, GBM, and libdrm.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Run these commands in the Hyprland session when XRLinuxDriver is already sending OpenTrack to `127.0.0.1:4242`. The renderer uses the current session and the supplied INI file as-is. [XRLinuxDriver OpenTrack output](https://xrlinuxdriver.readthedocs.io/en/latest/opentrack-app/)

```sh
./build/xr-pose --seconds 5
./build/xr-hyprland --config config.ini --debug
```

Move an application to the workspace on `XR-VIRTUAL` using your Hyprland bindings. Check `hyprctl monitors` for the actual output and workspace. Bind `xr-hyprlandctl recenter` to a key if desired. Run `./build/xr-hyprlandctl stop` to quit cleanly. The renderer removes its own headless output on normal exit and preserves an output it attached to.

Use `--flat` to run capture without a pose receiver, or `--preview --flat` to show capture in a diagnostic window. Whole-desktop mode uses DMA-BUF and can fall back to shared-memory capture with a GPU upload. Individual-window mode uses the standard ext-image-copy-capture protocols with shared memory and a GPU upload per window. The SHM path accepts 32-bit RGB formats and packed or padded RGB888/BGR888 buffers. The optional Breezy v5 pose backend requires the driver to expose that stream.

## Stereoscopy

Switch the glasses to their hardware SBS mode manually. At 3840×1080, the renderer draws a 1920×1080 view for each eye using eye origins separated by `ipd_mm`. It never changes the glasses' USB mode or physical monitor configuration. `stereo_mode=auto` requires agreement between the output geometry and fresh tracker state at `/dev/shm/xr_driver_state`. A mode mismatch produces black output and a log message until both agree. Confirmed 1920×1080 mono mode still works.

`ipd_mm=64` is a starting value, not a measurement of your eyes. Set a measured value with `xr-display ipd 63.5`, or adjust in 0.5 mm steps with `xr-display ipd +` / `xr-display ipd -`. Changes take effect immediately and persist. Accepted values are 45–80 mm. Recenter while looking forward using Super+Alt+R. `--stereo-preview --flat` provides a diagnostic SBS window without requiring hardware SBS or tracking.

## Individual windows

The supplied and installed INI select `view_mode=windows`. On service startup, XR automatically brings the currently visible workspace from each regular monitor onto a matching virtual output. It preserves the output resolution, scale, usable area, workspace tiling, and window sizes. The renderer maps those outputs back to the original desktop coordinates, keeping left/right and vertical placement. The largest logical display is centered in front of you. Turn your head to see the rest of the desktop; hidden workspaces are not piled into this initial view.

Whole-workspace transfer keeps the layout tree intact, including groups and fullscreen state. It also keeps applications rendering when the physical monitors sleep. Runtime rules apply only to the owned virtual outputs; the existing physical monitor configuration is unchanged. `~/.local/state/xr-hyprland/windows.json` records output identities and workspace assignments before the first move. Startup waits for every visible application's capture before blanking the regular monitors. A failed transfer triggers restoration through service cleanup.

**1440p applications:** a 2560×1440 desktop gets a 2560×1440 virtual output, and each application is captured at its actual buffer size. It is not squeezed into the default 1920×1080 XR workspace. The glasses still display 1920×1080 per eye in SBS, so native capture resolution does not add physical pixels. Enlarge an individual panel with `xr-display place --scale 1.3` for larger text. Many large windows increase the cost of SHM capture; requests are staggered across applications.

Each mapped visible application is captured separately. Super+Alt+M can send an additional focused application to the basic XR workspace; an application already brought into XR stays in place. Row/grid arrangements and placement changes affect only the XR presentation. `xr-display layout reset` returns to the native multi-monitor arrangement.

```sh
xr-display list
xr-display move --window 3
xr-display layout row
xr-display place --window 3 --yaw 15 --pitch 0 --distance 1.5 --scale 1.2
xr-display focus next
xr-display return --window 3
xr-display return all
```

Select an application by its current list index, Hyprland address, or Wayland foreign-toplevel identifier. Without a selector, actions use the focused application. Positive yaw places a panel to the left; positive pitch places it above you. Distance is in meters. `xr-display layout reset` restores the native arrangement. Panel placement persists while that application instance remains open, in `~/.local/state/xr-hyprland/windows.json`.

Stopping XR wakes the regular displays and returns the transferred workspaces to their original monitors. Their layout trees remain intact. If a physical monitor is disconnected, its matching virtual output and recovery record are retained for a later retry rather than discarding that workspace. `xr-display return all` returns the desktop and wakes the monitors while leaving the renderer running.

An application explicitly sent with Super+Alt+M has an individual restore record for its workspace and floating geometry; Super+Alt+Backspace returns it. An application brought along with its whole workspace returns with the desktop. `ExecStopPost` attempts both kinds of restoration after an unexpected renderer exit. Unique Wayland identifiers protect individual restore records from address reuse. For individually moved applications, tiled order, fullscreen state, and groups are not snapshotted.

Focus cycling also moves Hyprland's pointer to the native window center, allowing keyboard input and ordinary compositor controls. Pointer coordinates still follow the native headless layout: arbitrary spatial panel placement does not yet remap mouse motion or provide ray picking/3D dragging. Separate popup surfaces and compositor decorations may need additional handling. Use keyboard controls to arrange panels. Per-window SHM uploads cost more CPU bandwidth than desktop DMA-BUF capture.

`xr-display view desktop` switches back to a single captured desktop; `xr-display view windows` selects independent panels. Switching a running session restores its imported workspaces and re-sends individually moved XR applications. Independent-window mode imports the visible desktop again. Existing desktop bindings and layouts remain in place.

## Tracking and drift

The Hyprland client adapts Air Lab's speed-adaptive quaternion smoothing. Small head movements are smoothed more strongly than deliberate turns. Recenter immediately resets the filter. After a stale feed or a detected OpenTrack reference reset, the client resumes tracking while holding its current screen orientation. If `roll_enabled=false` is set in the supplied INI, the renderer levels the horizon while preserving the view direction.

To diagnose continuous rotation, leave the glasses stationary and run `./build/xr-pose --seconds 35` by itself. It reports the raw orientation and increments `reference_generation` when it detects an OpenTrack sender change or counter restart. Smoothing reduces jitter. Sustained rotation in the raw feed needs investigation at the tracker. Air Lab documents its timestamp and factory-calibration fixes and provides an optional project-local tracker build. Its launcher can use `--udp-port 4242` with the default Hyprland client settings. Run a single tracker at a time. [Drift investigation](air-lab/docs/STATUS.md#drift-investigation) · [Air Lab tracker setup](air-lab/docs/SETUP.md#tracker-build)

## Installed session controls

The local setup installs the binaries under `~/.local/bin`, a separate configuration at `~/.config/xr-hyprland/config.ini`, and the user service `xr-hyprland.service`. Its installed configuration uses `roll_enabled=true` for room anchoring. When you tilt your head, the image counter-rotates in the glasses to stay level with the room. Setting `roll_enabled=false` keeps the camera horizon level in the glasses, allowing the window to turn with your head relative to the room. Restart the renderer after changing the INI file. The existing `xr-driver.service` uses an override selecting the patched Air Lab tracker from `~/.local/share/xr-hyprland/tracker-da303d2`. Tracking sends OpenTrack to loopback port 4242. Driver metrics, the OpenTrack input listener, and Gamescope integration are disabled.

The Hyprland Lua configuration includes these controls.

| Shortcut | Action |
| --- | --- |
| Super+Alt+X | Toggle XR |
| Super+Alt+R | Recenter while looking forward |
| Super+Alt+M | Move the focused application to the XR workspace |
| Super+Alt+Escape | Stop XR and return its applications |
| Super+Alt+Backspace | Return an individually sent application |
| Super+Alt+Tab / Shift+Tab | Focus next / previous XR application |
| Super+Alt+G / Shift+G | Arrange panels in a row / grid |
| Super+Alt+Arrow keys | Move the focused panel by 5° |
| Super+Alt+Shift+Up / Down | Move the focused panel nearer / farther by 0.1 m |
| Super+Alt+Comma / Period | Decrease / increase IPD by 0.5 mm |

The same actions are available through `xr-display`; use `xr-display --help` for arguments and `xr-display status` for the current mode. `move` starts the renderer when needed, discovers the current virtual workspace, and sends the focused application there while keeping the current desktop in place. The renderer is started on demand. The session target stops it at logout. Logs are available with `journalctl --user -u xr-hyprland.service -f`.

The service waits for a captured frame with valid tracking, transfers the visible desktop in window mode, and waits for all imported window captures before turning off the regular physical monitors through [Hyprland DPMS](https://wiki.hypr.land/configuring/core/dispatchers/). The glasses and headless output stay on. This keeps monitor layouts, modes, and workspaces available while the screens sleep. Diagnostic previews leave monitors on. The renderer signals readiness using [systemd notification](https://www.freedesktop.org/software/systemd/man/latest/sd_notify.html); an unsuccessful start leaves the regular monitors on.

Stopping XR (including Super+Alt+Escape) wakes the monitors that XR turned off. The service's exit cleanup also attempts this after a crash, before restoring windows. `xr-display wake` wakes those monitors while keeping XR running. Displays already off before XR remain off, and restore records are matched by connector and device identity. Pending recovery lives in `~/.local/state/xr-hyprland/monitors.json` and is retried at the next start. No Hyprland configuration changes are needed for monitor power control.

Configuration backups are stored under `~/.local/share/xr-hyprland/backups/`. Only the marked XR controls block is added to the existing Hyprland Lua configuration. The latest installation manifest is `~/.local/share/xr-hyprland/install-manifest.json`.

## References and licensing

Application source is GPL-3.0-only. The bundled Wayland protocol definitions carry their own licenses in the XML files. The pose interfaces were checked against [XRLinuxDriver](https://github.com/wheaney/XRLinuxDriver/tree/36e2139dd482f5984112d0c3ebafcc07a78f99c0) and the coordinate convention against [Breezy Desktop](https://github.com/wheaney/breezy-desktop/tree/751b55ccb49d45448c44088f142a6af906afeffd).
