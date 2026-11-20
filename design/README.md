# Icon & banner sources

- `icon.svg` / `icon-dev.svg`: launcher icon (108×108 adaptive-icon canvas, glyph scaled to 78% because
  Google TV crops launcher circles tighter than the 66dp safe zone). The Android versions are hand-ported
  vectors: `app/src/main/res/drawable/ic_launcher_{foreground,background}.xml`, debug background in `app/src/debug/res`.
- `tv-banner.svg` / `tv-banner-dev.svg`: 320×180dp TV banner, exported to 640×360 PNG
  (`app/src/{main,debug}/res/drawable-xhdpi/tv_banner.png`).

Export on macOS (Quick Look renders SVG incl. text): `qlmanage -t -s 640 -o out file.svg`, then crop with `sips`.
Release = blue→violet gradient, Dev = orange→red with a DEV pill.
