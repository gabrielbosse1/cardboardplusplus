/*
 * Copyright 2019 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef CARDBOARD_PLUS_PLUS_ANDROID_SRC_MAIN_JNI_CARDBOARD_PLUS_PLUS_APP_H_
#define CARDBOARD_PLUS_PLUS_ANDROID_SRC_MAIN_JNI_CARDBOARD_PLUS_PLUS_APP_H_

#include <android/asset_manager.h>
#include <jni.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <GLES2/gl2.h>
#include "cardboard.h"
#include "util.h"

#include "VideoReceiver.h"
#include "H264Decoder.h"

namespace ndk_cardboardplusplus {

/**
 * Cardboard++ VR app. Renders camera passthrough or custom eye textures
 * (one per eye) through the Cardboard lens distortion pipeline.
 */
class CardboardPlusPlusApp {
 public:
  /**
   * Creates a CardboardPlusPlusApp.
   *
   * @param vm JavaVM pointer.
   * @param obj Android activity object.
   * @param asset_mgr_obj The asset manager object.
   */
  CardboardPlusPlusApp(JavaVM* vm, jobject obj, jobject asset_mgr_obj);

  ~CardboardPlusPlusApp();

  /**
   * Initializes any GL-related objects. This should be called on the rendering
   * thread with a valid GL context.
   *
   * @param env The JNI environment.
   */
  void OnSurfaceCreated(JNIEnv* env);

  /**
   * Sets screen parameters.
   *
   * @param width Screen width
   * @param height Screen height
   */
  void SetScreenParams(int width, int height);

  /**
   * Draws the scene. This should be called on the rendering thread.
   */
  void OnDrawFrame();

  /**
   * Toggles camera passthrough on/off.
   */
  void OnTriggerEvent();

  /**
   * Pauses head tracking.
   */
  void OnPause();

  /**
   * Resumes head tracking.
   */
  void OnResume();

  /**
   * Allows user to switch viewer.
   */
  void SwitchViewer();

  void DrawCameraQuad(GLuint texture_id);

  void OnCameraTextureInitialized(int textureId, int width, int height);

  int CreateCameraTexture();

  void ResetCameraTexture();

  void SetEyeTexture(int eye, int textureId);

  void StartVideoReceiver(int port);
  void StopVideoReceiver();
  bool HasVideoFrame();
  void UpdateVideoTexture();

  /**
   * Runs the FFmpeg decode loop off the GL render thread. Pulls H.264 packets
   * from the receiver, decodes them and publishes the newest RGBA frame into a
   * double-buffered slot consumed by UpdateVideoTexture. This keeps the
   * (expensive) software decode and YUV->RGBA conversion off the render
   * critical path so the GL thread only does the small texture upload.
   */
  void DecodeLoop();

 private:
  /**
   * Default near clip plane z-axis coordinate.
   */
  static constexpr float kZNear = 0.1f;

  /**
   * Default far clip plane z-axis coordinate.
   */
  static constexpr float kZFar = 100.f;

  /**
   * Updates device parameters, if necessary.
   *
   * @return true if device parameters were successfully updated.
   */
  bool UpdateDeviceParams();

  /**
   * Initializes GL environment.
   */
  void GlSetup();

  /**
   * Deletes GL environment.
   */
  void GlTeardown();

  /**
   * Gets head's pose as a 4x4 matrix.
   *
   * @return matrix containing head's pose.
   */
  Matrix4x4 GetPose();

  /**
   * Draws a 2D texture quad for the given eye.
   */
  void DrawEyeQuad(GLuint texture_id);

  jobject java_asset_mgr_;
  AAssetManager* asset_mgr_;

  CardboardHeadTracker* head_tracker_;
  CardboardLensDistortion* lens_distortion_;
  CardboardDistortionRenderer* distortion_renderer_;

  CardboardEyeTextureDescription left_eye_texture_description_;
  CardboardEyeTextureDescription right_eye_texture_description_;

  bool screen_params_changed_;
  bool device_params_changed_;
  int screen_width_;
  int screen_height_;

  float projection_matrices_[2][16];
  float eye_matrices_[2][16];

  GLuint depthRenderBuffer_;  // depth buffer
  GLuint framebuffer_;        // framebuffer object
  GLuint texture_;            // distortion texture

  GLuint tex2d_program_;
  GLuint tex2d_position_param_;
  GLuint tex2d_tex_coord_param_;
  GLuint tex2d_mvp_param_;
  GLuint tex2d_texture_param_;

  GLuint tex_program_;
  GLuint tex_position_param_;
  GLuint tex_tex_coord_param_;
  GLuint tex_mvp_param_;
  GLuint tex_texture_param_;

  GLuint left_eye_texture_;
  GLuint right_eye_texture_;
  GLuint left_eye_framebuffer_;
  GLuint right_eye_framebuffer_;

  GLuint camera_texture_;
  int camera_width_;
  int camera_height_;
  bool camera_texture_initialized_;
  bool show_camera_texture_;

  GLuint left_eye_custom_texture_;
  GLuint right_eye_custom_texture_;
  bool left_eye_texture_set_;
  bool right_eye_texture_set_;

  Matrix4x4 head_view_;

  TexturedMesh quad_;

  // Video streaming
  std::unique_ptr<VideoReceiver> video_receiver_;
  std::unique_ptr<H264Decoder> h264_decoder_;
  GLuint video_texture_;
  bool video_receiver_started_;
  int video_width_;
  int video_height_;

  // Decode thread + latest-frame handoff (decouples decode from the GL thread).
  std::thread decode_thread_;
  bool decode_thread_running_ = false;
  std::mutex video_frame_mutex_;
  // Two RGBA buffers the decode thread alternates between. The GL thread uploads
  // the published one directly (no extra copy), so the decode thread only
  // reuses a slot once the GL thread has released it (see held_slot_).
  std::shared_ptr<std::vector<uint8_t>> video_buf_a_;
  std::shared_ptr<std::vector<uint8_t>> video_buf_b_;
  std::shared_ptr<std::vector<uint8_t>> video_latest_;
  int video_latest_slot_ = -1;  // 0 = buf_a, 1 = buf_b, -1 = none
  int held_slot_ = -1;          // slot the GL thread is currently uploading from
  int video_latest_w_ = 0;
  int video_latest_h_ = 0;
  bool video_latest_ready_ = false;
  // Currently allocated GPU texture size (for glTexSubImage2D vs realloc).
  int video_tex_w_ = 0;
  int video_tex_h_ = 0;
};

}  // namespace ndk_cardboardplusplus

#endif  // CARDBOARD_PLUS_PLUS_ANDROID_SRC_MAIN_JNI_CARDBOARD_PLUS_PLUS_APP_H_
