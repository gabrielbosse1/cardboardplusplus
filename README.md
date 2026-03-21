# Cardboard++
## Bringing Google Cardboard Closer to a Meta Quest!

![License](https://img.shields.io/github/license/gabrielbosse1/cardboardplusplus)
![Stars](https://img.shields.io/github/stars/gabrielbosse1/cardboardplusplus)
![Last Commit](https://img.shields.io/github/last-commit/gabrielbosse1/cardboardplusplus)

I'm making this software with one goal: bringing features normally exclusive to expensive VR (like the Meta Quest) to a simple Google Cardboard.
These features include:

### Hand tracking

<p align="left"> <img width="500" src="assets/hand-tracking-demo.jpg"/> </p>

### 6DOF (degrees of freedom)

### SteamVR compatibility

### And even using Xbox controllers as virtual VR controllers!

---

The project is still a work in progress. But trust me, when you see the code, you will know there's a lot of work to do.
I'm working to implement the features I want and to make the project work. If you're a developer too, **all contributions are welcome.**

The original Android app was built using Google GVR, so it had to be fully rewritten using the newer Cardboard SDK.

This was necessary to avoid being locked to the default Cardboard viewer, which makes the app nearly unusable on most VR headsets.

For now, development is focused on building the bridge between the new Android app and the SteamVR driver.

## Current progress
- [x] **Camera preview**:
Low-latency passthrough implemented using the Camera2 API and rendered via OpenGL ES shaders to provide a "see-through" background.
- [x] **Hand tracking**:
Powered by MediaPipe Hand Landmarker. It currently tracks 21 points on each hand on-device, though it's CPU/GPU intensive and will eventually be moved to the PC driver side for better performance. (missing after rewriting the android app)
- [x] **6DoF (unstable)**:
Experimental 6-Degrees-of-Freedom tracking using the device's IMU (Linear Acceleration + Rotation Vector). Currently drifts by ~10-20cm during fast movements because it lacks visual odometry. (missing after rewriting the android app)
- [x] **SteamVR driver**:
A C++ OpenVR-based driver is ready in the `driver_cardboardplusplus` folder. It can be recognized by SteamVR, but the network bridge to receive data from the Android app is still in development. (coming soon)
- [ ] **Controller emulation**:
Planned support for using external gamepads (like Xbox or PS controllers) as virtual VR controllers, using the phone's tracking data to estimate their position in space.
