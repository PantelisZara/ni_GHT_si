# ni_GHT_si v0.1.0 — Experimental

Extract this Linux x86_64 archive and run `bin/night-monitor` in a graphical session. It requires compatible host Qt 6 libraries/QML plugins, the C++ runtime and Qt's Wayland plugin when using Wayland. Dependencies are not bundled; this is not a portable or static binary.

GPU collection is disabled by default. `--gpu` opts into NVIDIA telemetry when built with NVML support; an installed NVIDIA driver library is required. `--no-gpu` explicitly keeps it disabled. A stalled GPU driver can delay shutdown when enabled.

Use `--refresh-mode fast|balanced|low-power` or change profiles in Settings. `--smoke` runs bounded interface checks without the GPU driver. Process actions require a separate confirmation and supported native manager/broker metadata. Unsupported scopes remain protected.

Experimental and unmaintained. No support or further updates are planned. See the included README, release notes, MIT license and third-party notices. No superiority or universal Linux compatibility is claimed.
