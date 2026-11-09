# Third-party sources

SunjooAn Auto Chroma v1.3

- The stroke precision model, GPU precision shader and dock controls are independent additions for this project. They do not include Blackmagic Design code or claim to reproduce DaVinci Resolve's proprietary algorithms.

- `ac_keyer.effect`, `ac_plate_keyer.effect`, `ac_plate_clean.effect` and the Lua analysis modules come from the supplied `auto-chroma-v0.3` bundle. The reused files retain their original comments and attribution. The Lua modules and original notices are under `native/vendor/auto-chroma-v0.3`.
- The supplied shaders identify OBS Studio chroma key code as their source. Original OBS contributor notices remain in the source. See `upstream-license.md` and `GPL-2.0.txt` in the plugin data directory.
- This distribution's source code is provided under GNU GPL version 2. The complete corresponding source is in this workspace/source package. Existing upstream attribution remains applicable.
- OBS Studio/libobs and OBS frontend APIs: OBS Project contributors, GNU GPL v2. Runtime libraries are supplied by OBS, not bundled into this plugin package.
- LuaJIT: Mike Pall and contributors, MIT license. This plugin uses the LuaJIT runtime already included in OBS (`lua51.dll`); it does not redistribute it. Official source and license: https://github.com/LuaJIT/LuaJIT/blob/v2.1/COPYRIGHT
- Qt: The Qt Company and contributors. This plugin dynamically links to Qt libraries supplied by OBS and does not redistribute them. SDKs downloaded for building retain their own licenses. See https://www.qt.io/licensing/ and the SDK's license files.

OBS Studio source: https://github.com/obsproject/obs-studio

Original supplied notices: `native/vendor/auto-chroma-v0.3/license.md`.
