# Native USB camera viewer for Raspberry Pi 3

The C++17 viewer runs full screen on a Debian `armhf` Raspberry Pi 3 with a USB camera and HDMI touchscreen. It reads V4L2 buffers through `mmap`, decodes MJPEG into the display's usual BGRX byte order with libjpeg-turbo, then scales and writes to `/dev/fb0`. It falls back to YUYV when the camera lacks JPEG capture. It skips queued stale frames and avoids decoding or drawing while the backlight is off.

## Set up and build on the Pi

From the project root on your computer, copy the sources to the Pi. This setup uses `pi@hscam000.local` with SSH key authentication. Use the Pi's IP address if its hostname does not resolve.

```sh
ssh pi@hscam000.local "mkdir -p /home/pi/diy.usb_microcam"
scp camera.cpp camera.hpp display.cpp display.hpp main.cpp platform.cpp platform.hpp rect.hpp touch.cpp touch.hpp touch_test.cpp touch_diagnostic.py Makefile README.md .gitignore usb-microcam-native.service screenshots-smb.conf pi@hscam000.local:/home/pi/diy.usb_microcam/
ssh pi@hscam000.local
```

On the Pi, install the build dependencies and compile:

```sh
cd /home/pi/diy.usb_microcam
sudo apt update
sudo apt install build-essential libjpeg-dev libpng-dev v4l-utils
make -j2
```

Continue to [Start at boot without logging in](#start-at-boot-without-logging-in) to install the system service.

## Run manually

Use a Linux text console, or SSH in while the Pi's HDMI display is on a text console. The viewer takes over the active virtual console and restores it on Ctrl+C or SIGTERM. Keep that console active while viewing. It needs a Linux framebuffer with a packed 16-bit or 32-bit true-color mode. Stop the Python viewer and any desktop session using the display before starting it. `--no-vt` skips console mode changes when the framebuffer is already dedicated to the viewer.

The viewer finds a USB V4L2 camera and looks for a touchscreen whose name contains `waveshare` before falling back to other compatible touchscreens. On WaveShare panels it treats slot 0 as the primary contact because this panel reports each touch in slots 0 and 1. It retries if a device is unplugged. Use `./cam_viewer --help` for all options. For example:

```sh
sudo ./cam_viewer --device /dev/video0 --size 640x480 --fps 30 --fixed-exposure-fps
sudo ./cam_viewer --touch-name '' --idle 10
```

If the service is already running, use `sudo systemctl stop usb-microcam-native.service` before launching a manual instance. Start the service again after quitting the manual viewer.

`--touch /dev/input/eventN` selects a specific touchscreen. `--fill` crops to fill the display; the default fits the whole frame with black bars. `--no-osd` hides the ZOOM and FPS overlay.

## Touch and backlight

| Input | Action |
| --- | --- |
| Double tap | Cycle 1× → 2.5× → 5× → 1×, anchored at the tap. Zoom starts locked, so unlock first |
| Tap the lock icon (top right) | Lock or unlock zoom. The viewer starts locked: the icon is amber with a closed padlock and taps on the picture are ignored; only the reset and camera icons still respond |
| Tap the reset icon (bottom left) | Return to 1× and recenter the picture |
| Tap the camera icon (bottom right) | Save the current screen as a PNG. On success, the screen border flashes and the button shows a green checkmark. The button turns red on failure. Wait half a second between captures. |
| No touch for five minutes by default, or 15 minutes with the boot service | Turn off the LCD backlight |
| Touch while the backlight is off | Wake the display |

The app uses the same active-low BCM GPIO 26 wiring as `backlight.py`, via line 26 on `/dev/gpiochip0`. Use `--gpio-chip` or `--gpio-line` if your mapping differs. `--no-backlight` leaves the GPIO alone; `--idle 0` keeps the backlight on. If the touch device or GPIO line is unavailable, video still works and the app skips idle blanking.

## Camera controls and performance

In dim light, the Logitech C170 can lower its frame rate to allow longer exposures. The live adjustment we used on the Pi was:

```sh
v4l2-ctl -d /dev/video0 --set-ctrl=exposure_dynamic_framerate=0
v4l2-ctl -d /dev/video0 --get-ctrl=exposure_dynamic_framerate,auto_exposure
```

Use the active capture node if it differs from `/dev/video0`. The expected result on this camera is `exposure_dynamic_framerate: 0` and `auto_exposure: 3`, the C170's aperture-priority mode with automatic exposure time. This change can make the picture darker in low light. The viewer's `--fixed-exposure-fps` option applies the same setting whenever it opens the camera, including at startup and after a USB reconnect. The service already passes this flag.

Use [`v4l2-ctl`](https://manpages.debian.org/bookworm/v4l-utils/v4l2-ctl.1.en.html) to see what the camera supports and adjust settings such as exposure, focus, or brightness. Use the capture node shown in the viewer log. For example, if `128` is in the reported brightness range:

```sh
v4l2-ctl -d /dev/video0 --list-formats-ext
v4l2-ctl -d /dev/video0 --list-ctrls-menus
v4l2-ctl -d /dev/video0 --set-ctrl=brightness=128
```

The startup log shows the negotiated resolution and format. The FPS overlay shows how many fresh frames the viewer actually displays. On this Pi, the camera was configured for 30 FPS and the viewer showed about 25 FPS after the exposure change. If FPS still drops, check the Pi for power throttling with `vcgencmd get_throttled`. `--size 320x240` reduces the decode and USB load if needed.

## Start at boot without logging in

The included systemd system unit starts under `multi-user.target`, so no desktop session or user login is needed. It uses the stable device links for the Logitech C170 and WaveShare panel. Check the links on the Pi and edit `ExecStart` before installing if yours differ:

```sh
ls -l /dev/v4l/by-id/ /dev/input/by-id/
```

If you previously started a temporary service of the same name with `systemd-run`, stop it first with `sudo systemctl stop usb-microcam-native.service`.

The boot service requests 640×480 at 30 FPS, sets the backlight timeout to 15 minutes, and saves screenshots to `/home/pi/diy.usb_microcam/screenshots`. When run manually, the viewer uses `screenshots` in its working directory unless you pass `--screenshots DIR`.

From `/home/pi/diy.usb_microcam` on the Pi, run:

```sh
make -j2
sudo install -m 0755 cam_viewer /usr/local/bin/usb-microcam-native
sudo install -d -m 0750 -o pi -g pi /home/pi/diy.usb_microcam/screenshots
sudo install -m 0644 usb-microcam-native.service /etc/systemd/system/usb-microcam-native.service
sudo systemctl daemon-reload
sudo systemctl enable --now usb-microcam-native.service
```

Check that it is enabled at boot, running now, and applying the exposure setting:

```sh
systemctl is-enabled usb-microcam-native.service
systemctl is-active usb-microcam-native.service
sudo journalctl -b -u usb-microcam-native.service -n 30 --no-pager
v4l2-ctl -d /dev/v4l/by-id/usb-_Webcam_C170-video-index0 \
  --get-ctrl=exposure_dynamic_framerate,auto_exposure
```

Expect `enabled`, `active`, and a camera log entry that says `dynamic exposure frame rate disabled`. After rebuilding and reinstalling the binary, restart it with `sudo systemctl restart usb-microcam-native.service`. If you edit the unit, reinstall it and run `sudo systemctl daemon-reload` before restarting.

## Copy screenshots over SMB

On the Windows computer, open `\\hscam000.local\screenshots` in File Explorer. Sign in as `pi` with the Samba password. The writable share is limited to the Pi's local subnet. The viewer continues to save new PNGs into the shared folder.

On a fresh Samba installation, install the included configuration and create the SMB password on the Pi. Change `hosts allow` in `screenshots-smb.conf` if your LAN uses a different subnet. If the Pi already serves other SMB shares, merge the `[screenshots]` section into its existing configuration and apply the subnet restriction to that share.

```sh
cd /home/pi/diy.usb_microcam
sudo apt install samba
sudo cp -a /etc/samba/smb.conf /etc/samba/smb.conf.orig
sudo testparm -s screenshots-smb.conf
sudo install -m 0644 screenshots-smb.conf /etc/samba/smb.conf
sudo smbpasswd -a pi
sudo systemctl enable smbd.service
sudo systemctl restart smbd.service
```

## Gesture tests

`make test` runs the gesture tests without a camera, framebuffer, or physical touchscreen. To check the hardware path on the Pi, run with `--idle 10`, let the backlight turn off, tap to wake it, then unplug and reconnect the camera. Check that the picture returns and double taps still work.
