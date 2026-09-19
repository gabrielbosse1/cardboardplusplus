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
#include <cstdlib>
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

// Walks a serialized cardboard::DeviceParams proto2 message, finds the
// inter_lens_distance field (field 4, float), saves its value to *saved_ipd
// and overwrites it with 0.0f in place. Returns true if the field was found.
bool ZeroDeviceParamsIpd(uint8_t* data, int size, float* saved_ipd) {
  constexpr uint32_t kInterLensDistanceField = 4;
  constexpr uint32_t kWireTypeVarint = 0;
  constexpr uint32_t kWireTypeFixed64 = 1;
  constexpr uint32_t kWireTypeLengthDelimited = 2;
  constexpr uint32_t kWireTypeFixed32 = 5;

  auto read_varint = [data, size](int* pos, uint64_t* value) {
    uint64_t result = 0;
    int shift = 0;
    while (*pos < size && shift < 64) {
      uint8_t byte = data[*pos];
      (*pos)++;
      result |= static_cast<uint64_t>(byte & 0x7F) << shift;
      if (!(byte & 0x80)) {
        *value = result;
        return true;
      }
      shift += 7;
    }
    return false;
  };

  int pos = 0;
  while (pos < size) {
    uint64_t tag;
    if (!read_varint(&pos, &tag)) {
      return false;
    }
    const uint32_t field = static_cast<uint32_t>(tag >> 3);
    const uint32_t wire = static_cast<uint32_t>(tag & 0x7);
    switch (wire) {
      case kWireTypeVarint: {
        uint64_t unused;
        if (!read_varint(&pos, &unused)) {
          return false;
        }
        break;
      }
      case kWireTypeFixed64: {
        if (pos + 8 > size) {
          return false;
        }
        pos += 8;
        break;
      }
      case kWireTypeLengthDelimited: {
        uint64_t length;
        if (!read_varint(&pos, &length)) {
          return false;
        }
        if (length > static_cast<uint64_t>(size) ||
            pos + length > static_cast<uint64_t>(size)) {
          return false;
        }
        pos += static_cast<int>(length);
        break;
      }
      case kWireTypeFixed32: {
        if (pos + 4 > size) {
          return false;
        }
        if (field == kInterLensDistanceField) {
          std::memcpy(saved_ipd, data + pos, sizeof(float));
          const float zero = 0.0f;
          std::memcpy(data + pos, &zero, sizeof(float));
          return true;
        }
        pos += 4;
        break;
      }
      default:
        return false;
    }
  }
  return false;
}

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

// OES external texture sampler for camera passthrough and SBS video.
// u_UMin/u_UMax select the horizontal range of the SBS texture for each eye
// (left eye: 0.0-0.5, right eye: 0.5-1.0). u_VMax clamps V to skip macroblock
// padding rows at the bottom of H.264 frames.
constexpr const char* kTexFragmentShader =
    R"glsl(
    #extension GL_OES_EGL_image_external : require
    precision mediump float;
    varying vec2 v_TexCoord;
    uniform samplerExternalOES sTexture;
    uniform float u_VMax;
    uniform float u_UMin;
    uniform float u_UMax;
    void main() {
      float v = 1.0 - v_TexCoord.y * u_VMax;
      float u = u_UMin + v_TexCoord.x * (u_UMax - u_UMin);
      gl_FragColor = texture2D(sTexture, vec2(u, v));
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
}

CardboardPlusPlusApp::~CardboardPlusPlusApp() {
  decode_thread_running_ = false;
  if (video_receiver_) video_receiver_->Stop();
  if (decode_thread_.joinable()) {
    decode_thread_.join();
  }
  free(direct_buf_);
  direct_buf_ = nullptr;
  direct_buf_cap_ = 0;
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
  tex_vmax_param_ = glGetUniformLocation(tex_program_, "u_VMax");
  tex_umin_param_ = glGetUniformLocation(tex_program_, "u_UMin");
  tex_umax_param_ = glGetUniformLocation(tex_program_, "u_UMax");

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
  // Runs on the GL thread: retire the texture id stashed by StopVideoReceiver
  // before allocating a new one (deleting after regen could kill the live id).
  if (video_texture_pending_delete_) {
    glDeleteTextures(1, &video_texture_pending_delete_);
    video_texture_pending_delete_ = 0;
  }
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
    mid_feed_video_ = env->GetMethodID(cls, "feedDirect", "(Ljava/nio/ByteBuffer;IZ)V");
    env->DeleteLocalRef(cls);
    if (!mid_feed_video_) {
      LOGE("SetVideoDecoder: feedDirect method not found");
    }
  }
  LOGD("SetVideoDecoder: decoder=%p", (void*)video_decoder_obj_);
}

void CardboardPlusPlusApp::OnVideoActive() {
  video_active_ = true;
  LOGD("Video decoder active");
}

void CardboardPlusPlusApp::SetScreenParams(int width, int height) {
  screen_width_ = width;
  screen_height_ = height;
  screen_params_changed_ = true;
}

void CardboardPlusPlusApp::OnDrawFrame() {
  // Runs on the GL thread: drain a retired video texture when there is no
  // restart (no CreateVideoTexture) to delete it.
  if (video_texture_pending_delete_) {
    glDeleteTextures(1, &video_texture_pending_delete_);
    video_texture_pending_delete_ = 0;
  }
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
    DrawCameraQuad(camera_texture_, 0.0f, 1.0f);
  } else if (video_active_ && video_texture_) {
    // SBS video: left eye gets left half (0.0-0.5)
    DrawCameraQuad(video_texture_, 0.0f, 0.5f);
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
    DrawCameraQuad(camera_texture_, 0.0f, 1.0f);
  } else if (video_active_ && video_texture_) {
    // SBS video: right eye gets right half (0.5-1.0)
    DrawCameraQuad(video_texture_, 0.5f, 1.0f);
  } else if (right_eye_texture_set_) {
    DrawEyeQuad(right_eye_custom_texture_);
  } else {
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  }

  // Both camera passthrough and SBS video now draw a full eye view into each
  // eye's framebuffer (SBS split is done in the shader via u_UMin/u_UMax).
  // So the distortion renderer should sample the full texture (0.0-1.0) for
  // both eyes in all cases.
  left_eye_texture_description_.left_u = 0.0f;
  left_eye_texture_description_.right_u = 1.0f;
  right_eye_texture_description_.left_u = 0.0f;
  right_eye_texture_description_.right_u = 1.0f;

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

  // Save the viewer's real IPD (inter_lens_distance) before zeroing it so the
  // lens distortion is built without eye separation; SteamVR applies the IPD.
  std::vector<uint8_t> params(buffer, buffer + size);
  CardboardQrCode_destroy(buffer);
  if (ZeroDeviceParamsIpd(params.data(), static_cast<int>(params.size()),
                          &saved_ipd_meters_)) {
    LOGD("Saved viewer IPD %.3fm; lens distortion built with IPD = 0",
         saved_ipd_meters_);
  } else {
    LOGW("inter_lens_distance not found in device params; IPD left untouched");
  }

  CardboardLensDistortion_destroy(lens_distortion_);
  lens_distortion_ = CardboardLensDistortion_create(
      params.data(), static_cast<int>(params.size()), screen_width_,
      screen_height_);

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

void CardboardPlusPlusApp::DrawCameraQuad(GLuint texture_id, float u_min, float u_max) {
  glUseProgram(tex_program_);

  Matrix4x4 identity = GetIdentityMatrix();
  std::array<float, 16> identity_array = identity.ToGlArray();
  glUniformMatrix4fv(tex_mvp_param_, 1, GL_FALSE, identity_array.data());

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture_id);
  glUniform1i(tex_texture_param_, 0);
  glUniform1f(tex_vmax_param_, tex_vmax_value_);
  glUniform1f(tex_umin_param_, u_min);
  glUniform1f(tex_umax_param_, u_max);

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

  // Signal the forwarding thread first, then wake it out of its blocking
  // wait via receiver Stop() before joining (join-before-stop would hang).
  decode_thread_running_ = false;
  video_receiver_->Stop();
  if (decode_thread_.joinable()) {
    decode_thread_.join();
  }

  video_active_ = false;

  // Runs on the UI thread with no GL context: only stash the id. It is
  // deleted on the GL thread in CreateVideoTexture()/OnDrawFrame().
  if (video_texture_) {
    video_texture_pending_delete_ = video_texture_;
    video_texture_ = 0;
  }

  video_receiver_started_ = false;
  LOGD("Video receiver stopped");
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
    bool is_key = false;
    // Blocks on the receiver's condition variable (no 1ms spin); returns
    // false when Stop() drained the queue. is_key was computed once at
    // receive time, so no rescan here — Java only rescans keyframes for SPS.
    if (!video_receiver_->WaitAndGetFrame(&frame_data, &frame_size, &is_key)) {
      if (!decode_thread_running_) break;
      continue;
    }

    // Reused direct-buffer handoff: one malloc'd region (grown as needed),
    // wrapped per frame without copying into a JNI array.
    if ((size_t)frame_size > direct_buf_cap_) {
      uint8_t* grown =
          static_cast<uint8_t*>(realloc(direct_buf_, (size_t)frame_size));
      if (!grown) {
        LOGE("DecodeLoop: direct buffer realloc failed (%d)", frame_size);
        continue;
      }
      direct_buf_ = grown;
      direct_buf_cap_ = (size_t)frame_size;
    }
    memcpy(direct_buf_, frame_data, (size_t)frame_size);
    jobject buf = env->NewDirectByteBuffer(direct_buf_, (jlong)frame_size);
    if (!buf) {
      continue;
    }
    env->CallVoidMethod(video_decoder_obj_, mid_feed_video_, buf,
                        (jint)frame_size, is_key ? JNI_TRUE : JNI_FALSE);
    env->DeleteLocalRef(buf);

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

float CardboardPlusPlusApp::GetIpdMeters() const {
  return saved_ipd_meters_;
}

}  // namespace ndk_cardboardplusplus
