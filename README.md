# XR Hyprland

An experimental native Wayland client for displaying one Hyprland headless desktop on the original XREAL Air. It captures the virtual output with Wayland screencopy and uses OpenGL ES to project it as a plane. XRLinuxDriver supplies orientation.

**Status:** GPU capture and presentation worked at 1080p/60 on an RTX 3090 with Hyprland 0.56.2. A 90-second tracked trial rendered 5,398 frames with zero capture failures. Stable world anchoring is still unverified. The wearer reported drift, and a separate test saw the driver's orientation continue rotating with the renderer stopped. Stationary recalibration and a repeat wear test remain open. Physical hotplug recovery, cursor interaction, 120 Hz, and extended performance have not passed acceptance testing.

## Build and run

Install a C++20 compiler, CMake, pkg-config, nlohmann-json, Wayland development tools (including wayland-scanner), EGL, OpenGL ES, GBM, and libdrm.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

In the Hyprland session, enable XRLinuxDriver's OpenTrack output. This changes the driver's configured output mode. Restore your preferred driver mode after the session.

```sh
xr_driver_cli --opentrack-app
./build/xr-pose --seconds 5
./build/xr-hyprland --config config.ini --debug
```

Move an application to the workspace on `XR-VIRTUAL` using your Hyprland bindings. Check `hyprctl monitors` for the actual output and workspace. Bind `xr-hyprlandctl recenter` to a key if desired. Run `./build/xr-hyprlandctl stop` to quit cleanly. The renderer removes its own headless output on normal exit and preserves an output it attached to.

Use `--flat` to isolate capture from tracking, or `--preview --flat` to show capture in a diagnostic window. The default capture path uses DMA-BUF and can fall back to shared-memory capture with a GPU upload. PipeWire capture is not implemented. The optional Breezy v5 pose backend requires the driver to expose that stream. OpenTrack is the default because it worked with the tested driver installation.

## References and licensing

Application source is GPL-3.0-only. The bundled Wayland protocol definitions carry their own licenses in the XML files. The pose interfaces were checked against [XRLinuxDriver](https://github.com/wheaney/XRLinuxDriver/tree/36e2139dd482f5984112d0c3ebafcc07a78f99c0) and the coordinate convention against [Breezy Desktop](https://github.com/wheaney/breezy-desktop/tree/751b55ccb49d45448c44088f142a6af906afeffd).
