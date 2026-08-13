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

#include "cardboardplusplus_app.h"

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>

#include <array>
#include <cmath>
#include <cstring>
#include <fstream>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "cardboard.h"

namespace ndk_cardboardplusplus {

namespace {

// 6 Hz cutoff frequency for the velocity filter of the head tracker.
constexpr int kVelocityFilterCutoffFrequency = 6;

constexpr uint64_t kPredictionTimeWithoutVsyncNanos = 50000000;

constexpr const char* kTexVertexShader =
    R"glsl(
    uniform mat4 u_MVPMatrix;
    attribute vec4 a_Position;
    attribute vec2 a_TexCoord;
    varying vec2 v_TexCoord;
    void main() {
      gl_Position = u_MVPMatrix * a_Position;
      v_TexCoord = a_TexCoord;
    })glsl";

// OES external texture sampler for camera passthrough
constexpr const char* kTexFragmentShader =
    R"glsl(
    #extension GL_OES_EGL_image_external : require
    precision mediump float;
    varying vec2 v_TexCoord;
    uniform samplerExternalOES sTexture;
    void main() {
      gl_FragColor = texture2D(sTexture, vec2(v_TexCoord.x, 1.0 - v_TexCoord.y));
    })glsl";

// Regular 2D texture sampler for eye textures
constexpr const char* k2DTexFragmentShader =
    R"glsl(
    precision mediump float;
    varying vec2 v_TexCoord;
    uniform sampler2D sTexture;
    void main() {
      gl_FragColor = texture2D(sTexture, v_TexCoord);
    })glsl";

}  // anonymous namespace

CardboardPlusPlusApp::CardboardPlusPlusApp(JavaVM* vm, jobject obj,
                                     jobject asset_mgr_obj)
    : head_tracker_(nullptr),
      lens_distortion_(nullptr),
      distortion_renderer_(nullptr),
      screen_params_changed_(false),
      device_params_changed_(false),
      screen_width_(0),
      screen_height_(0),
      depthRenderBuffer_(0),
      framebuffer_(0),
      texture_(0),
      tex2d_program_(0),
      tex2d_position_param_(0),
      tex2d_tex_coord_param_(0),
      tex2d_mvp_param_(0),
      tex2d_texture_param_(0),
      camera_texture_(0),
      camera_width_(0),
      camera_height_(0),
      camera_texture_initialized_(false),
      show_camera_texture_(false),
      left_eye_custom_texture_(0),
      right_eye_custom_texture_(0),
      left_eye_texture_set_(false),
      right_eye_texture_set_(false),
      video_texture_(0),
      video_receiver_started_(false),
      video_width_(2880),
      video_height_(1620) {
  JNIEnv* env;
  vm->GetEnv((void**)&env, JNI_VERSION_1_6);
  java_asset_mgr_ = env->NewGlobalRef(asset_mgr_obj);
  asset_mgr_ = AAssetManager_fromJava(env, asset_mgr_obj);

  Cardboard_initializeAndroid(vm, obj);
  head_tracker_ = CardboardHeadTracker_create();
  CardboardHeadTracker_setLowPassFilter(head_tracker_,
                                        kVelocityFilterCutoffFrequency);

  video_receiver_ = std::make_unique<VideoReceiver>();
  h264_decoder_ = std::make_unique<H264Decoder>();
}

CardboardPlusPlusApp::~CardboardPlusPlusApp() {
  decode_thread_running_ = false;
  if (decode_thread_.joinable()) {
    decode_thread_.join();
  }
  if (video_texture_ != 0) {
    glDeleteTextures(1, &video_texture_);
    video_texture_ = 0;
  }
  CardboardHeadTracker_destroy(head_tracker_);
  CardboardLensDistortion_destroy(lens_distortion_);
  CardboardDistortionRenderer_destroy(distortion_renderer_);
}

void CardboardPlusPlusApp::OnSurfaceCreated(JNIEnv* env) {
  const int tex_vertex_shader = LoadGLShader(GL_VERTEX_SHADER, kTexVertexShader);

  // Camera OES texture program
  const int tex_fragment_shader = LoadGLShader(GL_FRAGMENT_SHADER, kTexFragmentShader);
  tex_program_ = glCreateProgram();
  glAttachShader(tex_program_, tex_vertex_shader);
  glAttachShader(tex_program_, tex_fragment_shader);
  glLinkProgram(tex_program_);
  glUseProgram(tex_program_);

  CHECKGLERROR("Tex program");

  tex_position_param_ = glGetAttribLocation(tex_program_, "a_Position");
  tex_tex_coord_param_ = glGetAttribLocation(tex_program_, "a_TexCoord");
  tex_mvp_param_ = glGetUniformLocation(tex_program_, "u_MVPMatrix");
  tex_texture_param_ = glGetUniformLocation(tex_program_, "sTexture");

  CHECKGLERROR("Tex program params");

  // 2D texture program for eye textures
  const int tex2d_fragment_shader = LoadGLShader(GL_FRAGMENT_SHADER, k2DTexFragmentShader);
  tex2d_program_ = glCreateProgram();
  glAttachShader(tex2d_program_, tex_vertex_shader);
  glAttachShader(tex2d_program_, tex2d_fragment_shader);
  glLinkProgram(tex2d_program_);
  glUseProgram(tex2d_program_);

  CHECKGLERROR("Tex2D program");

  tex2d_position_param_ = glGetAttribLocation(tex2d_program_, "a_Position");
  tex2d_tex_coord_param_ = glGetAttribLocation(tex2d_program_, "a_TexCoord");
  tex2d_mvp_param_ = glGetUniformLocation(tex2d_program_, "u_MVPMatrix");
  tex2d_texture_param_ = glGetUniformLocation(tex2d_program_, "sTexture");

  CHECKGLERROR("Tex2D program params");

  CARDBOARDPLUSPLUS_CHECK(quad_.Initialize(tex_position_param_, tex_tex_coord_param_,
                                        "Quad.obj", asset_mgr_));

  // (Re)create the video texture here. The GL context is destroyed and recreated
  // on every pause/resume, which invalidates previously allocated texture ids,
  // so this must run on each surface (re)creation rather than only once in
  // StartVideoReceiver. This is what kept the SBS frame frozen after an onResume.
  if (video_texture_ != 0) {
    glDeleteTextures(1, &video_texture_);
    video_texture_ = 0;
  }
  glGenTextures(1, &video_texture_);
  glBindTexture(GL_TEXTURE_2D, video_texture_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  if (video_width_ > 0 && video_height_ > 0) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, video_width_, video_height_, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    video_tex_w_ = video_width_;
    video_tex_h_ = video_height_;
  }

  CHECKGLERROR("OnSurfaceCreated");
}

void CardboardPlusPlusApp::SetScreenParams(int width, int height) {
  screen_width_ = width;
  screen_height_ = height;
  screen_params_changed_ = true;
}

void CardboardPlusPlusApp::OnDrawFrame() {
  if (!UpdateDeviceParams()) {
    return;
  }

  // Update Head Pose.
  head_view_ = GetPose();

  glEnable(GL_DEPTH_TEST);
  glEnable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  // Draw left eye
  glBindFramebuffer(GL_FRAMEBUFFER, left_eye_framebuffer_);
  glViewport(0, 0, screen_width_ / 2, screen_height_);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  if (camera_texture_initialized_ && show_camera_texture_) {
    DrawCameraQuad(camera_texture_);
  } else if (left_eye_texture_set_) {
    DrawEyeQuad(left_eye_custom_texture_);
  } else {
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  }

  // Draw right eye
  glBindFramebuffer(GL_FRAMEBUFFER, right_eye_framebuffer_);
  glViewport(0, 0, screen_width_ / 2, screen_height_);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  if (camera_texture_initialized_ && show_camera_texture_) {
    DrawCameraQuad(camera_texture_);
  } else if (right_eye_texture_set_) {
    DrawEyeQuad(right_eye_custom_texture_);
  } else {
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  }

  // The eye texture descriptions default to a side-by-side split (left half for
  // the left eye, right half for the right eye) which is correct for SBS video.
  // The camera passthrough draws the full image into each eye's framebuffer, so
  // for that case we must sample the whole texture in both eyes and let Cardboard
  // handle the IPD via the distortion meshes.
  const bool camera_pass =
      camera_texture_initialized_ && show_camera_texture_;
  if (camera_pass) {
    left_eye_texture_description_.left_u = 0.0f;
    left_eye_texture_description_.right_u = 1.0f;
    right_eye_texture_description_.left_u = 0.0f;
    right_eye_texture_description_.right_u = 1.0f;
  } else {
    left_eye_texture_description_.left_u = 0.0f;
    left_eye_texture_description_.right_u = 0.5f;
    right_eye_texture_description_.left_u = 0.5f;
    right_eye_texture_description_.right_u = 1.0f;
  }

  // Render with distortion
  CardboardDistortionRenderer_renderEyeToDisplay(
      distortion_renderer_, /* target_display = */ 0, /* x = */ 0, /* y = */ 0,
      screen_width_, screen_height_, &left_eye_texture_description_,
      &right_eye_texture_description_);

  CHECKGLERROR("onDrawFrame");
}

void CardboardPlusPlusApp::OnTriggerEvent() {
  show_camera_texture_ = !show_camera_texture_;
}

void CardboardPlusPlusApp::OnPause() {
  CardboardHeadTracker_pause(head_tracker_);
}

void CardboardPlusPlusApp::OnResume() {
  CardboardHeadTracker_resume(head_tracker_);

  // Parameters may have changed.
  device_params_changed_ = true;

  // Check for device parameters existence in external storage. If they're
  // missing, we must scan a Cardboard QR code and save the obtained parameters.
  uint8_t* buffer;
  int size;
  CardboardQrCode_getSavedDeviceParams(&buffer, &size);
  if (size == 0) {
    SwitchViewer();
  }
  CardboardQrCode_destroy(buffer);
}

void CardboardPlusPlusApp::SwitchViewer() {
  CardboardQrCode_scanQrCodeAndSaveDeviceParams();
}

bool CardboardPlusPlusApp::UpdateDeviceParams() {
  // Checks if screen or device parameters changed
  if (!screen_params_changed_ && !device_params_changed_) {
    return true;
  }

  // Get saved device parameters
  uint8_t* buffer;
  int size;
  CardboardQrCode_getSavedDeviceParams(&buffer, &size);

  // If there are no parameters saved yet, returns false.
  if (size == 0) {
    return false;
  }

  CardboardLensDistortion_destroy(lens_distortion_);
  lens_distortion_ = CardboardLensDistortion_create(buffer, size, screen_width_,
                                                    screen_height_);

  CardboardQrCode_destroy(buffer);

  GlSetup();

  CardboardDistortionRenderer_destroy(distortion_renderer_);
  const CardboardOpenGlEsDistortionRendererConfig config{kGlTexture2D};
  distortion_renderer_ = CardboardOpenGlEs2DistortionRenderer_create(&config);

  CardboardMesh left_mesh;
  CardboardMesh right_mesh;
  CardboardLensDistortion_getDistortionMesh(lens_distortion_, kLeft,
                                            &left_mesh);
  CardboardLensDistortion_getDistortionMesh(lens_distortion_, kRight,
                                            &right_mesh);

  CardboardDistortionRenderer_setMesh(distortion_renderer_, &left_mesh, kLeft);
  CardboardDistortionRenderer_setMesh(distortion_renderer_, &right_mesh,
                                      kRight);

  // Get eye matrices
  CardboardLensDistortion_getEyeFromHeadMatrix(lens_distortion_, kLeft,
                                               eye_matrices_[0]);
  CardboardLensDistortion_getEyeFromHeadMatrix(lens_distortion_, kRight,
                                               eye_matrices_[1]);
  CardboardLensDistortion_getProjectionMatrix(lens_distortion_, kLeft, kZNear,
                                              kZFar, projection_matrices_[0]);
  CardboardLensDistortion_getProjectionMatrix(lens_distortion_, kRight, kZNear,
                                              kZFar, projection_matrices_[1]);

  screen_params_changed_ = false;
  device_params_changed_ = false;

  CHECKGLERROR("UpdateDeviceParams");

  return true;
}

void CardboardPlusPlusApp::GlSetup() {
  LOGD("GL SETUP");

  if (framebuffer_ != 0) {
    GlTeardown();
  }

  glGenTextures(1, &left_eye_texture_);
  glBindTexture(GL_TEXTURE_2D, left_eye_texture_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, screen_width_ / 2, screen_height_, 0,
               GL_RGB, GL_UNSIGNED_BYTE, 0);

  glGenTextures(1, &right_eye_texture_);
  glBindTexture(GL_TEXTURE_2D, right_eye_texture_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, screen_width_ / 2, screen_height_, 0,
               GL_RGB, GL_UNSIGNED_BYTE, 0);

  left_eye_texture_description_.texture = left_eye_texture_;
  // The incoming video is side-by-side stereo: left half = left eye view,
  // right half = right eye view. Select each eye's half of the texture here,
  // otherwise both eyes are shown the full SBS image and every object appears
  // doubled.
  left_eye_texture_description_.left_u = 0.0f;
  left_eye_texture_description_.right_u = 0.5f;
  left_eye_texture_description_.top_v = 1.0f;
  left_eye_texture_description_.bottom_v = 0.0f;

  right_eye_texture_description_.texture = right_eye_texture_;
  right_eye_texture_description_.left_u = 0.5f;
  right_eye_texture_description_.right_u = 1.0f;
  right_eye_texture_description_.top_v = 1.0f;
  right_eye_texture_description_.bottom_v = 0.0f;

  glGenRenderbuffers(1, &depthRenderBuffer_);
  glBindRenderbuffer(GL_RENDERBUFFER, depthRenderBuffer_);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, screen_width_ / 2,
                        screen_height_);
  CHECKGLERROR("Create Render buffer");

  glGenFramebuffers(1, &framebuffer_);
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         left_eye_texture_, 0);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                            GL_RENDERBUFFER, depthRenderBuffer_);

  glGenFramebuffers(1, &left_eye_framebuffer_);
  glBindFramebuffer(GL_FRAMEBUFFER, left_eye_framebuffer_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                        left_eye_texture_, 0);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                            GL_RENDERBUFFER, depthRenderBuffer_);

  glGenFramebuffers(1, &right_eye_framebuffer_);
  glBindFramebuffer(GL_FRAMEBUFFER, right_eye_framebuffer_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                        right_eye_texture_, 0);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                            GL_RENDERBUFFER, depthRenderBuffer_);

  CHECKGLERROR("GlSetup");
}

void CardboardPlusPlusApp::GlTeardown() {
  if (framebuffer_ == 0) {
    return;
  }
  glDeleteRenderbuffers(1, &depthRenderBuffer_);
  depthRenderBuffer_ = 0;
  glDeleteFramebuffers(1, &framebuffer_);
  framebuffer_ = 0;
  glDeleteTextures(1, &texture_);
  texture_ = 0;
  glDeleteFramebuffers(1, &left_eye_framebuffer_);
  left_eye_framebuffer_ = 0;
  glDeleteFramebuffers(1, &right_eye_framebuffer_);
  right_eye_framebuffer_ = 0;
  glDeleteTextures(1, &left_eye_texture_);
  left_eye_texture_ = 0;
  glDeleteTextures(1, &right_eye_texture_);
  right_eye_texture_ = 0;

  CHECKGLERROR("GlTeardown");
}

Matrix4x4 CardboardPlusPlusApp::GetPose() {
  std::array<float, 4> out_orientation;
  std::array<float, 3> out_position;
  CardboardHeadTracker_getPose(
      head_tracker_, GetBootTimeNano() + kPredictionTimeWithoutVsyncNanos,
      kLandscapeLeft, &out_position[0], &out_orientation[0]);
  return GetTranslationMatrix(out_position) *
         Quatf::FromXYZW(&out_orientation[0]).ToMatrix();
}

void CardboardPlusPlusApp::DrawEyeQuad(GLuint texture_id) {
  glUseProgram(tex2d_program_);

  Matrix4x4 identity = GetIdentityMatrix();
  std::array<float, 16> identity_array = identity.ToGlArray();
  glUniformMatrix4fv(tex2d_mvp_param_, 1, GL_FALSE, identity_array.data());

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, texture_id);
  glUniform1i(tex2d_texture_param_, 0);

  LOGD("DrawEyeQuad: texture=%d, program=%d", texture_id, tex2d_program_);

  quad_.Draw();

  CHECKGLERROR("DrawEyeQuad");
}

void CardboardPlusPlusApp::DrawCameraQuad(GLuint texture_id) {
  glUseProgram(tex_program_);

  Matrix4x4 identity = GetIdentityMatrix();
  std::array<float, 16> identity_array = identity.ToGlArray();
  glUniformMatrix4fv(tex_mvp_param_, 1, GL_FALSE, identity_array.data());

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture_id);
  glUniform1i(tex_texture_param_, 0);

  quad_.Draw();

  CHECKGLERROR("DrawCameraQuad");
}

void CardboardPlusPlusApp::OnCameraTextureInitialized(int textureId, int width, int height) {
  camera_texture_ = static_cast<GLuint>(textureId);
  camera_width_ = width;
  camera_height_ = height;
  camera_texture_initialized_ = true;
  LOGD("Camera texture initialized: id=%d, size=%dx%d", textureId, width, height);
}

int CardboardPlusPlusApp::CreateCameraTexture() {
  GLuint textureId = 0;
  glGenTextures(1, &textureId);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, textureId);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  LOGD("Created camera texture: id=%d", textureId);
  return static_cast<int>(textureId);
}

void CardboardPlusPlusApp::ResetCameraTexture() {
  camera_texture_initialized_ = false;
  LOGD("Camera texture reset for pause");
}

void CardboardPlusPlusApp::SetEyeTexture(int eye, int textureId) {
  if (eye == 0) {
    left_eye_custom_texture_ = static_cast<GLuint>(textureId);
    left_eye_texture_set_ = (textureId != 0);
    LOGD("Left eye texture set: id=%d", textureId);
  } else if (eye == 1) {
    right_eye_custom_texture_ = static_cast<GLuint>(textureId);
    right_eye_texture_set_ = (textureId != 0);
    LOGD("Right eye texture set: id=%d", textureId);
  }
}

void CardboardPlusPlusApp::StartVideoReceiver(int port) {
  if (video_receiver_started_) {
    LOGD("Video receiver already started");
    return;
  }

  LOGD("Starting video receiver on port %d", port);

  video_receiver_->Start(port);
  video_receiver_started_ = true;

  // The video texture itself is (re)created in OnSurfaceCreated because the GL
  // context is recreated on every pause/resume, which invalidates it.
  h264_decoder_->Initialize(video_width_, video_height_);

  // Run H.264 decode + YUV->RGBA on a dedicated thread so the GL render thread
  // is never blocked by the (expensive) software decoder.
  decode_thread_running_ = true;
  decode_thread_ = std::thread(&CardboardPlusPlusApp::DecodeLoop, this);

  LOGD("Video receiver started, texture=%d", video_texture_);
}

void CardboardPlusPlusApp::StopVideoReceiver() {
  if (!video_receiver_started_) {
    return;
  }

  LOGD("Stopping video receiver");

  decode_thread_running_ = false;
  if (decode_thread_.joinable()) {
    decode_thread_.join();
  }

  video_receiver_->Stop();
  h264_decoder_->Shutdown();

  if (video_texture_) {
    glDeleteTextures(1, &video_texture_);
    video_texture_ = 0;
    video_tex_w_ = 0;
    video_tex_h_ = 0;
  }

  {
    std::lock_guard<std::mutex> lock(video_frame_mutex_);
    video_latest_.reset();
    video_buf_a_.reset();
    video_buf_b_.reset();
    video_latest_ready_ = false;
  }

  video_receiver_started_ = false;
  LOGD("Video receiver stopped");
}

bool CardboardPlusPlusApp::HasVideoFrame() {
  if (!video_receiver_started_) return false;
  return video_receiver_->HasFrame();
}

void CardboardPlusPlusApp::UpdateVideoTexture() {
  if (!video_receiver_started_ || !video_texture_) return;

  // The heavy H.264 decode + YUV->RGBA conversion now runs on the dedicated
  // decode thread (see DecodeLoop). Here on the GL thread we only grab the
  // newest decoded frame and upload it. glTexSubImage2D reuses the texture
  // storage allocated in OnSurfaceCreated instead of reallocating ~18 MB every
  // frame (the old glTexImage2D path stalled the GPU pipeline each tick).
  std::shared_ptr<std::vector<uint8_t>> frame;
  int w = 0;
  int h = 0;
  int slot = -1;
  {
    std::lock_guard<std::mutex> lock(video_frame_mutex_);
    if (!video_latest_ready_) {
      return;  // No new decoded frame; keep showing the last one.
    }
    frame = video_latest_;
    w = video_latest_w_;
    h = video_latest_h_;
    slot = video_latest_slot_;
    video_latest_ready_ = false;
    // Claim this slot so the decode thread won't overwrite it mid-upload.
    held_slot_ = slot;
  }

  if (!frame || frame->empty() || w <= 0 || h <= 0) {
    std::lock_guard<std::mutex> lock(video_frame_mutex_);
    held_slot_ = -1;
    return;
  }

  // Reallocate GPU texture storage only when the stream resolution changes.
  if (w != video_tex_w_ || h != video_tex_h_) {
    LOGD("Video texture size changed: %dx%d -> %dx%d, reallocating",
         video_tex_w_, video_tex_h_, w, h);
    glBindTexture(GL_TEXTURE_2D, video_texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    video_tex_w_ = w;
    video_tex_h_ = h;
  }

  // Upload directly from the published buffer (no intermediate copy).
  glBindTexture(GL_TEXTURE_2D, video_texture_);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE,
                  frame->data());

  SetEyeTexture(0, video_texture_);
  SetEyeTexture(1, video_texture_);

  // Release the slot so the decode thread can reuse it.
  {
    std::lock_guard<std::mutex> lock(video_frame_mutex_);
    held_slot_ = -1;
  }
}

void CardboardPlusPlusApp::DecodeLoop() {
  LOGD("Decode loop started");
  while (decode_thread_running_) {
    uint8_t* frame_data = nullptr;
    int frame_size = 0;
    if (!video_receiver_->GetFrame(&frame_data, &frame_size)) {
      // No packet queued yet; yield briefly instead of busy-spinning.
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }

    if (!h264_decoder_->DecodePacket(frame_data, frame_size)) {
      continue;
    }
    if (!h264_decoder_->HasDecodedFrame()) {
      continue;
    }

    int width = 0;
    int height = 0;
    int rgba_size = 0;
    uint8_t* rgba_data = nullptr;
    if (!h264_decoder_->GetRGBAFrame(&rgba_data, &rgba_size, &width, &height)) {
      continue;
    }

    // Handle a stream resolution change by reinitializing the decoder.
    if (width != video_width_ || height != video_height_) {
      LOGD("Frame dimensions changed: %dx%d -> %dx%d, reinitializing decoder",
           video_width_, video_height_, width, height);
      h264_decoder_->Shutdown();
      if (!h264_decoder_->Initialize(width, height)) {
        LOGE("Failed to reinitialize decoder with new dimensions");
        continue;
      }
      video_width_ = width;
      video_height_ = height;
    }

    // Write into whichever slot the GL thread is NOT currently holding as latest,
    // then publish it. The GL thread uploads the published buffer directly, so we
    // must not reuse that slot until the GL thread releases it (held_slot_).
    const int other_idx = (video_latest_slot_ == 0) ? 1 : 0;
    std::shared_ptr<std::vector<uint8_t>>& other =
        (other_idx == 0) ? video_buf_a_ : video_buf_b_;
    const size_t need = static_cast<size_t>(width) * height * 4;
    if (!other || other->size() != need) {
      other = std::make_shared<std::vector<uint8_t>>(need);
    }
    memcpy(other->data(), rgba_data, need);

    {
      std::unique_lock<std::mutex> lock(video_frame_mutex_);
      // Wait until the GL thread has finished uploading from this slot.
      while (held_slot_ == other_idx) {
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        lock.lock();
      }
      video_latest_ = other;
      video_latest_slot_ = other_idx;
      video_latest_w_ = width;
      video_latest_h_ = height;
      video_latest_ready_ = true;
    }
  }
  LOGD("Decode loop ended");
}

}  // namespace ndk_cardboardplusplus
