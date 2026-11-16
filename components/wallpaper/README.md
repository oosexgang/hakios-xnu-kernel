# The wallpaper

What Darwin 27 shows behind the lock screen and the home screen. A new device gets the "hero" poster,
and on this phone that is `com.apple.MercuryPoster`, a 3D scene drawn with Metal: a black screen when
Metal does not draw it. What is here gives Darwin a wallpaper it can draw: one of the Stripe collection
(Stripe Yellow unless `WALLPAPER=` names another colour), drawn by CoreAnimation layers for any screen.

| Where | Inside | What it does |
|---|---|---|
| `inject_poster.c` | `PosterBoard` | Picks the WallpaperKit collections poster as the first wallpaper, showing the device's default wallpaper, where PosterBoard would take Mercury. |
| `inject_wallpaper.m` | `CollectionsPoster` | Draws the Stripe wallpaper's six stripes once, by the processor, into a picture the layer shows, instead of six shape layers the window server would draw by the path renderer every frame (boot-arg `wallpaper_raster`). |

The default wallpaper itself is chosen when the image is made:
[`darwin/tools/mkimage26.py`](../../darwin/tools/mkimage26.py) `default_wallpaper()` writes Stripe Yellow
(or `WALLPAPER=green`, `orange`, `red`, `purple`, `blue`, `black`) into Darwin's default wallpaper list.
`darwin/userland/inj26.sh` builds both libraries, `darwin/userland/root26-darwin.sh` puts them into the
two processes.
