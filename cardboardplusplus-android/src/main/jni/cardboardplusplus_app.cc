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
  java_vm_ = vm;
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

  // The video texture is an OES texture owned by a Java SurfaceTexture (fed by
  // MediaCodec). It is created on demand from Java via CreateVideoTexture()
  // once the GL context is current, so nothing to allocate here.

  CHECKGLERROR("OnSurfaceCreated");
}

int CardboardPlusPlusApp::CreateVideoTexture() {
  GLuint textureId = 0;
  // Only allocate the OES texture name. It must NOT be bound to a GL context
  // here: SurfaceTexture(int) requires an unbound texture, and a pre-bound
  // texture conflicts with the MediaCodec producer connection on c2.qti.
  glGenTextures(1, &textureId);
  video_texture_ = textureId;
  LOGD("Created video (OES) texture: id=%d", textureId);
  return static_cast<int>(textureId);
}

void CardboardPlusPlusApp::SetVideoDecoder(JNIEnv* env, jobject decoder) {
  if (video_decoder_obj_) {
    env->DeleteGlobalRef(video_decoder_obj_);
    video_decoder_obj_ = nullptr;
  }
  mid_feed_video_ = nullptr;
  if (decoder) {
    video_decoder_obj_ = env->NewGlobalRef(decoder);
    jclass cls = env->GetObjectClass(decoder);
    mid_feed_video_ = env->GetMethodID(cls, "feedFrame", "([BZ)V");
    env->DeleteLocalRef(cls);
    if (!mid_feed_video_) {
      LOGE("SetVideoDecoder: feedFrame method not found");
    }
  }
  LOGD("SetVideoDecoder: decoder=%p", (void*)video_decoder_obj_);
}

void CardboardPlusPlusApp::OnVideoActive() {
  video_active_ = true;
  LOGD("Video decoder active");
}

bool CardboardPlusPlusApp::IsKeyframe(const uint8_t* data, int size) {
  for (int i = 0; i + 3 < size; ++i) {
    if (data[i] == 0x00 && data[i + 1] == 0x00 && (data[i + 2] & 0xFF) <= 0x01) {
      int sc = (data[i + 2] == 0x00 && i + 3 < size && data[i + 3] == 0x01)
                   ? 4
                   : (data[i + 2] == 0x01 ? 3 : 0);
      if (sc > 0) {
        int type = data[i + sc] & 0x1F;
        if (type == 5 || type == 7) return true;
        i += sc;
      }
    }
  }
  return false;
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
  } else if (video_active_ && video_texture_) {
    // Video is decoded by MediaCodec into a SurfaceTexture (OES), updated by
    // VideoDecoder.updateVideoTexture() just before this draw. Draw it through
    // the OES program; the SBS split is handled by the distortion eye ranges
    // (camera_pass == false below).
    DrawCameraQuad(video_texture_);
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
  } else if (video_active_ && video_texture_) {
    DrawCameraQuad(video_texture_);
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

  // Run the frame-forwarding loop on a dedicated thread. It pulls H.264 access
  // units from the receiver and hands them to the Java MediaCodec decoder via
  // JNI; the actual decode + GPU upload happen there, off the GL thread.
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

  video_active_ = false;

  if (video_texture_) {
    glDeleteTextures(1, &video_texture_);
    video_texture_ = 0;
  }

  video_receiver_started_ = false;
  LOGD("Video receiver stopped");
}

bool CardboardPlusPlusApp::HasVideoFrame() {
  if (!video_receiver_started_) return false;
  return video_receiver_->HasFrame();
}

void CardboardPlusPlusApp::UpdateVideoTexture() {
  // The video texture is now an OES texture owned by a Java SurfaceTexture fed
  // by MediaCodec. The GL thread calls SurfaceTexture.updateTexImage() (via
  // VideoDecoder.updateVideoTexture in Java) before onDrawFrame, which draws
  // the texture directly. No CPU upload happens here anymore.
}

void CardboardPlusPlusApp::DecodeLoop() {
  LOGD("Decode loop started");

  // Attach this native thread to the JVM so we can call the Java MediaCodec
  // wrapper via JNI.
  JNIEnv* env = nullptr;
  bool attached = false;
  if (java_vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) ==
      JNI_EDETACHED) {
    if (java_vm_->AttachCurrentThread(&env, nullptr) == JNI_OK) {
      attached = true;
    }
  }

  if (!env || !video_decoder_obj_ || !mid_feed_video_) {
    LOGE("DecodeLoop: missing JNI env/decoder, cannot forward frames");
    if (attached) java_vm_->DetachCurrentThread();
    return;
  }

  while (decode_thread_running_) {
    uint8_t* frame_data = nullptr;
    int frame_size = 0;
    if (!video_receiver_->GetFrame(&frame_data, &frame_size)) {
      // No packet queued yet; yield briefly instead of busy-spinning.
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }

    const bool is_key = IsKeyframe(frame_data, frame_size);

    jbyteArray arr = env->NewByteArray(frame_size);
    if (!arr) {
      continue;
    }
    env->SetByteArrayRegion(arr, 0, frame_size,
                            reinterpret_cast<jbyte*>(frame_data));
    env->CallVoidMethod(video_decoder_obj_, mid_feed_video_, arr,
                        is_key ? JNI_TRUE : JNI_FALSE);
    env->DeleteLocalRef(arr);

    if (env->ExceptionCheck()) {
      env->ExceptionDescribe();
      env->ExceptionClear();
    }
  }

  if (attached) {
    java_vm_->DetachCurrentThread();
  }
  LOGD("Decode loop ended");
}

}  // namespace ndk_cardboardplusplus
