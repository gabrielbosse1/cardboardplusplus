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
    // Impersonate knuckles the same way Valve's handskeletonsimulation sample
    // does: SteamVR then loads the full Index input profile (skeleton paths,
    // legacy bindings, icons) and animates the hand from skeletal input.
    VRProperties()->SetStringProperty(props,
        Prop_ControllerType_String, "knuckles");
    VRProperties()->SetStringProperty(props,
        Prop_InputProfilePath_String,
        "{indexcontroller}/input/index_controller_profile.json");
    VRProperties()->SetStringProperty(props,
        Prop_RenderModelName_String,
        m_handId == 0 ? "valve_controller_knu_1_0_left"
                      : "valve_controller_knu_1_0_right");
    // Create the skeletal input component
    EVRInputError err = VRDriverInput()->CreateSkeletonComponent(
        props,
        kSkeletonInputPath[m_handId],
        kSkeletonPath[m_handId],
        "/pose/raw",
        VRSkeletalTracking_Full,
        nullptr, // NULL = default grip limit
        kBoneCount,
        &m_skeletalHandle);
    if (err != VRInputError_None) {
        DriverLog("HandDriver: CreateSkeletonComponent failed for hand %d, err=%d", m_handId, err);
        return VRInitError_Init_Internal;
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
    // Inactive devices report no pose, so direct SteamVR polls between
    // despawn and respawn never see a frozen hand.
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
        // No data yet: report connected but no pose (waiting for first frame)
        m_pose.poseIsValid = false;
        m_pose.result = TrackingResult_Running_OK;
        m_pose.deviceIsConnected = true;
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
    // Push skeleton to SteamVR (both WithController and WithoutController ranges)
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
