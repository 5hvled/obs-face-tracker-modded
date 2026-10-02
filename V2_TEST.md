# V2 cleanup test

Experimental branch: `test/v2-cleanup`. Do not replace the stable release until OBS testing passes.

Changes: format the tracker sources; hide tracked scenes/sources when tracking is lost; apply opacity to both PNG and OBS overlays using a packaged shader; interpolate roll across the angle boundary; clear empty target selections; reject tracked sources/scenes containing the tracker or its filter parent; stop render re-entry; avoid stale overlay textures after render failure.

## Windows build

The V2 Test Windows Build workflow uploads a `v2-test-windows-obs30-x64` artifact. It uses the existing OBS 30 build dependencies and does not publish a release.

Close OBS before replacing the plugin. Keep a backup of the currently working DLL and data folder. Install the test DLL and its data together, including `face-prop.effect`, using the same layout as the existing plugin. Restore the backed-up files to roll back.

## Manual validation

- Test the filter and Face Tracker input source with your normal camera.
- Test PNG and tracked-source overlays at opacity 0, 50, and 100; check transparent edges.
- Leave and re-enter the camera with Hide When Lost on and off.
- Check position, size, offsets, manual rotation, and smooth head roll.
- Clear the input camera/source selection; the previous source must stop rendering.
- Select the tracker itself, its filtered camera, and a scene or nested scene containing it as the tracked overlay; OBS must remain responsive.
- Test two tracker instances referencing one another; OBS must remain responsive.
- Delete the tracked source; check switching scenes, opening settings, and closing OBS.

CI compilation cannot validate rendering, visual appearance, or OBS runtime stability. Those require testing in OBS.
