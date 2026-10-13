#include "HandDriver.h"
#include "HmdDriver.h"
#include "DriverLog.h"
#include <cstring>
using namespace vr;
// Skeleton paths for left/right hands
static const char* kSkeletonPath[2] = { "/skeleton/hand/left", "/skeleton/hand/right" };
static const char* kSkeletonInputPath[2] = { "/input/skeleton/left", "/input/skeleton/right" };
static const char* kSerialLeft = "CBPP_HAND_LEFT";
static const char* kSerialRight = "CBPP_HAND_RIGHT";
// OpenVR bone count (must match bridge's NUM_BONES)
static constexpr int kBoneCount = 31;
// Controller type decides which binding file SteamVR loads for a device, for its own menus, for
// SteamVR Home, for the Steam client and for every game. knuckles is the only type that has a
// skeleton-aware binding shipped for all of them at once (VRChat, SteamVR Home and the
// compositor all bind /skeleton/hand/* for it). Bindings always reference the leaf paths of a
// component (/input/a/click, /input/trigger/value, /input/grip/force), never the bare profile
// parent, so every component this device publishes is created at its leaf path.
// oculus_touch was tried and reverted: it wins nothing visually, and without a render model
// SteamVR falls back to its root vr_controller_vive_1_5 and draws a Vive wand where the hand is.
// SteamVR's own UI cannot draw a hand either way - its compositor action manifest declares no
// skeleton action at all - so the type is chosen purely to keep bindings resolving.
static const char* kControllerType = "knuckles";
static const char* kInputProfile = "{indexcontroller}/input/index_controller_profile.json";
// Render model Valve's knuckles profile expects, as a driver-qualified path. The path must carry
// the resource root or SteamVR looks for it under this driver's own root, finds nothing, and draws
// nothing at all - which is different from, and much worse than, omitting the property (that falls
// back to the global resources/rendermodels/vr_controller_vive_1_5 and draws a Vive wand).
static const char* kRenderModel[2] = {
    "{indexcontroller}/rendermodels/valve_controller_knu_1_0_left",
    "{indexcontroller}/rendermodels/valve_controller_knu_1_0_right",
};
HandDriver::HandDriver(HmdDriver* hmd, int handId)
    : m_hmd(hmd), m_handId(handId)
{
    std::memset(&m_pose, 0, sizeof(m_pose));
    std::memset(m_bones, 0, sizeof(m_bones));
    // Initialize all bones to identity quaternion
    for (int i = 0; i < kBoneCount; i++) {
        m_bones[i].orientation.w = 1.0;
        m_bones[i].orientation.x = 0.0;
        m_bones[i].orientation.y = 0.0;
        m_bones[i].orientation.z = 0.0;
    }
    const GripLimitBone* grip = (m_handId == 0) ? kGripLimitLeft : kGripLimitRight;
    for (int i = 0; i < kBoneCount; i++) {
        m_gripLimit[i].position.v[0] = grip[i].pos[0];
        m_gripLimit[i].position.v[1] = grip[i].pos[1];
        m_gripLimit[i].position.v[2] = grip[i].pos[2];
        m_gripLimit[i].orientation.w = grip[i].rot[0];
        m_gripLimit[i].orientation.x = grip[i].rot[1];
        m_gripLimit[i].orientation.y = grip[i].rot[2];
        m_gripLimit[i].orientation.z = grip[i].rot[3];
    }
}
EVRInitError HandDriver::Activate(uint32_t unObjectId)
{
    m_deviceId = unObjectId;
    PropertyContainerHandle_t props =
        VRProperties()->TrackedDeviceToPropertyContainer(m_deviceId);
    VRProperties()->SetStringProperty(props,
        Prop_ModelNumber_String, "CardboardHandTracker");
    VRProperties()->SetStringProperty(props,
        Prop_ManufacturerName_String, "CardboardPlusPlus");
    VRProperties()->SetStringProperty(props,
        Prop_SerialNumber_String,
        m_handId == 0 ? kSerialLeft : kSerialRight);
    VRProperties()->SetInt32Property(props,
        Prop_ControllerRoleHint_Int32,
        m_handId == 0
            ? TrackedControllerRole_LeftHand
            : TrackedControllerRole_RightHand);
    VRProperties()->SetStringProperty(props,
        Prop_TrackingSystemName_String, "cardboardplusplus");
    VRProperties()->SetStringProperty(props,
        Prop_ControllerType_String, kControllerType);
    VRProperties()->SetStringProperty(props,
        Prop_InputProfilePath_String, kInputProfile);
    VRProperties()->SetStringProperty(props,
        Prop_RenderModelName_String, kRenderModel[m_handId]);
    // The grip limit has to be this
    // device's own full fist, not the default: SteamVR reads it as both the
    // curl/splay reference and the ceiling on skeletal motion, and the default
    // is the Index controller's fist, which bottoms out before a bare hand
    // does and so stops MCP flexion short of the palm.
    EVRInputError err = VRDriverInput()->CreateSkeletonComponent(
        props,
        kSkeletonInputPath[m_handId],
        kSkeletonPath[m_handId],
        "/pose/raw",
        VRSkeletalTracking_Full,
        m_gripLimit,
        kBoneCount,
        &m_skeletalHandle);
    if (err != VRInputError_None) {
        // Degrade to a pose-only device rather than failing Activate: without
        // this the whole hand would disappear.
        m_skeletalHandle = 0;
        DriverLog("HandDriver: CreateSkeletonComponent failed for hand %d, err=%d; publishing without skeleton", m_handId, err);
    }
    m_active.store(true, std::memory_order_release);
    DriverLog("HandDriver: activated hand %d (%s), skeletal component created",
              m_handId, m_handId == 0 ? "left" : "right");
    return VRInitError_None;
}
void HandDriver::Deactivate()
{
    m_active.store(false, std::memory_order_release);
    m_deviceId = k_unTrackedDeviceIndexInvalid;
    // A deactivated device reports nothing, so the pose never outlives the
    // registration it belongs to.
    m_pose.poseIsValid = false;
    m_pose.result = TrackingResult_Running_OutOfRange;
    m_pose.deviceIsConnected = false;
    DriverLog("HandDriver: deactivated hand %d", m_handId);
}
void HandDriver::EnterStandby() {}
void* HandDriver::GetComponent(const char* pchComponentNameAndVersion)
{
    return nullptr;
}
void HandDriver::DebugRequest(const char* pchRequest, char* pchResponseBuffer, uint32_t unResponseBufferSize)
{
    if (unResponseBufferSize >= 1) pchResponseBuffer[0] = 0;
}
DriverPose_t HandDriver::GetPose()
{
    return m_pose;
}
void HandDriver::RunFrame()
{
    if (!m_active.load(std::memory_order_acquire)) return;
    // Read latest skeleton data from HmdDriver
    HmdDriver::SkeletonHand handData;
    if (!m_hmd->GetSkeletonHand(m_handId, handData)) {
        // No live skeleton (none yet, or the hand left the camera past the staleness
        // gate): publish a disconnected pose. That is the OpenVR signal games read as
        // "device put away" - SteamVR drops it from bindings and fires the usual
        // deactivate/reactivate events when the hand returns - which is far cleaner
        // than handing apps a frozen hand.
        m_pose.poseIsValid = false;
        m_pose.result = TrackingResult_Running_OutOfRange;
        m_pose.deviceIsConnected = false;
        VRServerDriverHost()->TrackedDevicePoseUpdated(m_deviceId, m_pose, sizeof(DriverPose_t));
        return;
    }
    // Convert bridge skeleton data to VRBoneTransform_t. Bones arrive
    // parent-relative (position offset + orientation in the parent frame),
    // matching Valve's handskeletonsimulation sample driver convention.
    for (int i = 0; i < kBoneCount && i < HmdDriver::SKELETON_BONE_COUNT; i++) {
        m_bones[i].position.v[0] = handData.bones[i].pos[0];
        m_bones[i].position.v[1] = handData.bones[i].pos[1];
        m_bones[i].position.v[2] = handData.bones[i].pos[2];
        m_bones[i].orientation.w = handData.bones[i].rot[0];
        m_bones[i].orientation.x = handData.bones[i].rot[1];
        m_bones[i].orientation.y = handData.bones[i].rot[2];
        m_bones[i].orientation.z = handData.bones[i].rot[3];
    }
    // Device pose is tracking-space truth composed by the bridge
    // (head rotation already applied to bone 0): copy it verbatim.
    m_pose.poseIsValid = true;
    m_pose.result = TrackingResult_Running_OK;
    m_pose.deviceIsConnected = true;
    m_pose.qWorldFromDriverRotation.w = 1.0;
    m_pose.qWorldFromDriverRotation.x = 0.0;
    m_pose.qWorldFromDriverRotation.y = 0.0;
    m_pose.qWorldFromDriverRotation.z = 0.0;
    m_pose.qDriverFromHeadRotation.w = 1.0;
    m_pose.qDriverFromHeadRotation.x = 0.0;
    m_pose.qDriverFromHeadRotation.y = 0.0;
    m_pose.qDriverFromHeadRotation.z = 0.0;
    m_pose.vecPosition[0] = m_bones[0].position.v[0];
    m_pose.vecPosition[1] = m_bones[0].position.v[1];
    m_pose.vecPosition[2] = m_bones[0].position.v[2];
    m_pose.qRotation.w = m_bones[0].orientation.w;
    m_pose.qRotation.x = m_bones[0].orientation.x;
    m_pose.qRotation.y = m_bones[0].orientation.y;
    m_pose.qRotation.z = m_bones[0].orientation.z;
    VRServerDriverHost()->TrackedDevicePoseUpdated(m_deviceId, m_pose, sizeof(DriverPose_t));
    // Root must be (0, identity) in skeleton space: its translation and
    // rotation are already baked into the device pose above.
    m_bones[0].position.v[0] = 0.0f;
    m_bones[0].position.v[1] = 0.0f;
    m_bones[0].position.v[2] = 0.0f;
    m_bones[0].orientation.w = 1.0f;
    m_bones[0].orientation.x = 0.0f;
    m_bones[0].orientation.y = 0.0f;
    m_bones[0].orientation.z = 0.0f;
    // Push skeleton to SteamVR (both WithController and WithoutController ranges);
    // the m_skeletalHandle guard covers the case where component creation failed
    // at Activate.
    if (m_skeletalHandle) {
        vr::EVRInputError skelErr = VRDriverInput()->UpdateSkeletonComponent(
            m_skeletalHandle,
            VRSkeletalMotionRange_WithController,
            m_bones, kBoneCount);
        if (skelErr != vr::VRInputError_None) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                DriverLog("HandDriver: UpdateSkeletonComponent failed hand=%d err=%d", m_handId, (int)skelErr);
            }
        }
        VRDriverInput()->UpdateSkeletonComponent(
            m_skeletalHandle,
            VRSkeletalMotionRange_WithoutController,
            m_bones, kBoneCount);
    }
}

