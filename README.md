# Cardboard++
## Bringing Google Cardboard closer to a Meta Quest

![License](https://img.shields.io/github/license/gabrielbosse1/cardboardplusplus)
![Stars](https://img.shields.io/github/stars/gabrielbosse1/cardboardplusplus)
![Last Commit](https://img.shields.io/github/last-commit/gabrielbosse1/cardboardplusplus)

The project goal is: bring features normally exclusive to expensive VR headsets (like the Meta Quest) to a simple Google Cardboard. Features include hand tracking, 6DoF, SteamVR compatibility, and using Xbox controllers as virtual VR controllers. Hand tracking provides their position in space, while the controller's inputs are mapped to SteamVR.

---

## How it works

The project has two main parts:

### Android app (`cardboardplusplus-android/`)

A VR app built with the Google Cardboard SDK that runs on your phone inside a headset.

- **Camera passthrough**: Low-latency preview using the Camera2 API, rendered via
  OpenGL ES shaders to provide a see-through background.
- **Hand tracking**: Powered by MediaPipe Hand Landmarker. Tracks 21 points per hand
  on-device. *(Not yet ported to the new app, in progress)*
- **6DoF tracking**: Experimental 6-Degrees-of-Freedom using the device's IMU
  (linear acceleration + rotation vector). Drifts ~10-20cm during fast movements
  without visual odometry. *(Not yet ported to the new app, in progress)*

### SteamVR driver (`driver_cardboardplusplus/`)

A C++ OpenVR-based driver that makes SteamVR recognize your phone as a VR headset.

- **HMD driver**: Provides head tracking data to SteamVR.
- **Controller driver**: Maps input from external gamepads (Xbox, PS, etc.) as
  virtual VR controllers.
- **Video encoding**: Encodes and streams VR video to the Android app.
- **Network bridge** *(in progress)*: Connects the Android app to the driver over
  a local network to send tracking data and receive video.

---

## Current progress

- [x] Camera passthrough (Android app)
- [x] SteamVR driver (HMD, controller, video encoding)
- [ ] Hand tracking (needs porting to new app)
- [ ] 6DoF tracking (needs porting to new app)
- [ ] Network bridge between app and driver
- [ ] External gamepad as VR controllers

---

## Contributing

This is a work in progress. **All contributions are welcome.**
See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

## License

[Apache 2.0](LICENSE)
