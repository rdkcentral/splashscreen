# RDKE OpenGL ES Splash (JPEG/PNG)

A small splash-screen application intended for RDK/RDKE-style environments using Essos/Westeros.

## Behavior

- Displays a JPEG or PNG image fullscreen using OpenGL ES 2.0.
- Keeps running until a **dismiss file** exists.
  - Default dismiss file: `/tmp/.dismissSplash`
- Image file path can be passed via CLI (expected to be part of the firmware image).
  - If no `--image` is provided (or decoding fails), a solid-color fallback splash is shown.

## Build

```sh
mkdir -p build
cd build
cmake ..
make
```

This produces the `rdke_splash` binary.

## Run

```sh
./rdke_splash --image /opt/splash/splash.png --dismiss-file /tmp/.dismissSplash
```

Run without specifying an image (uses fallback splash):

```sh
./rdke_splash --dismiss-file /tmp/.dismissSplash
```

To dismiss:

```sh
touch /tmp/.dismissSplash
```

## systemd

See `rdke_splash.service` and adjust the image path to match your platform.

Useful commands:

```sh
systemctl start rdke_splash.service
systemctl stop rdke_splash.service
systemctl status rdke_splash.service
```

Notes:

- The unit starts `rdke_splash` without CLI arguments (legacy-style); the app will show the fallback splash unless you change the unit to pass `--image ...`.
- `dismiss_splash.sh` touches `/tmp/.dismissSplash` to dismiss the splash.

## Notes

- Requires: `essos`, `westeros_gl`, `EGL`, `GLESv2`, `libjpeg`, `libpng`, `zlib`.
- The service uses `LD_PRELOAD=libwesteros_gl.so.0.0.0` to select the proper GL integration layer.

### Image scaling

- The image is fit to the display while preserving aspect ratio (letterbox/pillarbox as needed).

