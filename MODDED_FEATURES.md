# Scenes Modded

This fork contains the custom OBS Face Tracker build used for the modded Windows plugin.

### Added functionality

- Face Prop Overlay using a PNG image.
- Prop scale, X/Y offset, opacity, smoothing, rotation, face-size following, and head-tilt following.
- Hide prop when face tracking is lost.
- Tracked Source / Scene selection from OBS sources.
- Tracked source rendering positioned according to the detected face.

The Windows build was tested successfully with CMake/Visual Studio and produced a working obs-face-tracker.dll.

The original project is by Norihiro and remains GPLv2. This fork is intended for personal/community use and preserves the upstream license.
