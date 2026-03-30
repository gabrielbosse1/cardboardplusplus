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

#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <GLES2/gl2.h>
#include "cardboard.h"
#include "util.h"
#include "NetworkClient.h"

namespace ndk_cardboardplusplus {

/**
 * This is a sample app for the Cardboard SDK. It loads a simple environment and
 * objects that you can click on.
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
   * Hides the target object if it's being targeted.
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

  void DrawTexQuad(GLuint texture_id);

  void DrawCameraQuad(GLuint texture_id);

  void OnCameraTextureInitialized(int textureId, int width, int height);

  int CreateCameraTexture();

  void ResetCameraTexture();

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
   * Draws all world-space objects for the given eye.
   */
  void DrawWorld();

  /**
   * Draws the target object.
   */
  void DrawTarget();

  /**
   * Draws the room.
   */
  void DrawRoom();

  /**
   * Finds a new random position for the target object.
   */
  void HideTarget();

  /**
   * Checks if user is pointing or looking at the target object by calculating
   * whether the angle between the user's gaze and the vector pointing towards
   * the object is lower than some threshold.
   *
   * @return true if the user is pointing at the target object.
   */
  bool IsPointingAtTarget();

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

  GLuint obj_program_;
  GLuint obj_position_param_;
  GLuint obj_uv_param_;
  GLuint obj_modelview_projection_param_;

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

  Matrix4x4 head_view_;
  Matrix4x4 model_target_;

  Matrix4x4 modelview_projection_target_;
  Matrix4x4 modelview_projection_room_;

  TexturedMesh room_;
  Texture room_tex_;

  std::vector<TexturedMesh> target_object_meshes_;
  std::vector<Texture> target_object_not_selected_textures_;
  std::vector<Texture> target_object_selected_textures_;
  int cur_target_object_;

  TexturedMesh quad_;

  // Network client for discovery, video, tracking, camera
  cbpp::NetworkClient networkClient_;
};

}  // namespace ndk_cardboardplusplus

#endif  // CARDBOARD_PLUS_PLUS_ANDROID_SRC_MAIN_JNI_CARDBOARD_PLUS_PLUS_APP_H_
