# The camera (and the flashlight)

The phone's cameras as Darwin's capture stack wants them, and the flashlight that the same stack serves.
Darwin expects the iPhone's ISP and Apple's capture device made of it; here the kernel's camera driver
gives the frames (`/dev/hakios-camera`) and the flashlight's level (`/dev/hakios-torch`), and what is here
makes a capture device of ours out of them inside the process that wants one.

| Where | What |
|---|---|
| `src/camera_device.c` | A FigCaptureDevice with two FigCaptureStreams: the wide camera as the back camera, the front one as the front camera, in place of the device Apple's H10ISP makes. Darwin 16.5's formats, or the iPhone 11's with `HAKIOS_IOS27`. |
| `src/camera_isp.c`, `camera_isp.h` | The picture processor: the sensor's raw frames to the pictures the stream gives (demosaic, white balance, colour), with the sensor's colour tuning when the image has one. |
| `src/inject_capture.c` | Inside `cameracaptured`, Darwin 27's camera daemon: the device above for the capture stack, and the flashlight of Control Center and AVFlashlight to `/dev/hakios-torch`. |
| `src/inject_camera_app.c` | Inside Camera.app: the preview's picture given a size where AVFoundation, with one camera and no base zoom factor, gives it none. |
| `src/camera_test.c`, `camera_avtest.m` | The processor on a frame from the kernel, and AVFoundation's view of the cameras. |
| `tools/camsensor.py` | The sensor's register tables out of Android's module file (`camera-<sensor>.tab`, read by the kernel's driver). |
| `tools/camtuning.py` | The sensor's colour tuning out of Android's tuning file (`camera-<sensor>.isp`, read by the processor). |
| `tools/camraw.py` | Pictures of the raw frames a run hands back. |

On Darwin 16.5 the device is part of `libhakios-media.dylib` in `mediaserverd`
(`userland/userland/src/inject_media.c`, built by `userland/userland/build.sh`); on Darwin 27
`darwin/userland/inj26.sh` builds `libhakios-capture.dylib` and `libhakios-camapp.dylib` from `src/`.

## In the kernel

[`kernel/overlay/bsd/dev/board/camera.c`](../../kernel/overlay/bsd/dev/board/camera.c) starts the
sensors and the camera's interface from the board file (`camera { }`, a `sensor@N { }` each) and hands
the frames to `/dev/hakios-camera`; [`kernel/overlay/bsd/dev/board/torch.c`](../../kernel/overlay/bsd/dev/board/torch.c)
is the flashlight.
