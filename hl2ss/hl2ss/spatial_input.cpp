
#include "locator.h"
#include "utilities.h"
#include "spatial_input.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.Input.Spatial.h>
#include <winrt/Windows.Perception.h>
#include <winrt/Windows.Perception.Spatial.h>
#include <winrt/Windows.Perception.People.h>

using namespace winrt::Windows::Foundation::Numerics;
using namespace winrt::Windows::UI::Input;
using namespace winrt::Windows::UI::Input::Spatial;
using namespace winrt::Windows::Perception;
using namespace winrt::Windows::Perception::Spatial;
using namespace winrt::Windows::Perception::People;

//-----------------------------------------------------------------------------
// Global Variables
//-----------------------------------------------------------------------------

static HANDLE g_event_consent = NULL;
static HANDLE g_thread_consent = NULL;
static GazeInputAccessStatus g_status_consent = GazeInputAccessStatus::Unspecified;
static SpatialInteractionManager g_sim = nullptr;
static std::vector<HandJointKind> g_joints;

//-----------------------------------------------------------------------------
// Functions
//-----------------------------------------------------------------------------

// OK
static DWORD WINAPI SpatialInput_RequestEyeAccess(void* param)
{
    (void)param;

    g_status_consent = EyesPose::RequestAccessAsync().get();
    SetEvent(g_event_consent);

    return 0;
}

// OK
bool SpatialInput_WaitForEyeConsent()
{
    WaitForSingleObject(g_event_consent, INFINITE);
    return g_status_consent == GazeInputAccessStatus::Allowed;
}

// OK
void SpatialInput_Initialize()
{
    g_event_consent = CreateEvent(NULL, TRUE, FALSE, NULL);
    g_thread_consent = CreateThread(NULL, 0, SpatialInput_RequestEyeAccess, NULL, 0, NULL);

    g_joints.resize(HAND_JOINTS);
    for (int i = 0; i < HAND_JOINTS; ++i) { g_joints[i] = (HandJointKind)i; }

    g_sim = SpatialInteractionManager::GetForCurrentView();
}

// OK
int SpatialInput_GetHeadPoseAndEyeRay(SpatialCoordinateSystem const& world, PerceptionTimestamp const& ts, Frame& head_pose, Ray& eye_ray)
{
    SpatialPointerPose pointer = SpatialPointerPose::TryGetAtTimestamp(world, ts);
    int ret = 0;

    if (!pointer) { return ret; }

    auto h = pointer.Head();

    head_pose.position = h.Position();
    head_pose.forward  = h.ForwardDirection();
    head_pose.up       = h.UpDirection();

    ret |= 1;

    auto pose = pointer.Eyes();
    if (!pose) { return ret; }

    auto gaze = pose.Gaze();
    if (!gaze) { return ret; }

    auto spatialRay = gaze.Value();

    eye_ray.origin    = spatialRay.Origin;
    eye_ray.direction = spatialRay.Direction;

    ret |= 2;

    return ret;
}

// OK
int SpatialInput_GetHandPose(SpatialCoordinateSystem const& world, PerceptionTimestamp const& ts, std::vector<JointPose>& left_poses, std::vector<JointPose>& right_poses)
{
    auto source_states = g_sim.GetDetectedSourcesAtTimestamp(ts);
    int ret = 0;
    std::vector<JointPose>* target;
    int id;    
    bool ok;

    for (auto source_state : source_states)
    {
    if (source_state.Source().Kind() != SpatialInteractionSourceKind::Hand) { continue; }

    switch (source_state.Source().Handedness())
    {
    case SpatialInteractionSourceHandedness::Left:  target = &left_poses;  id = 1; break;
    case SpatialInteractionSourceHandedness::Right: target = &right_poses; id = 2; break;
    default: continue;
    }

    auto pose = source_state.TryGetHandPose();
    if (!pose) { continue; }

    ok = pose.TryGetJoints(world, g_joints, *target);
    if (!ok) { continue; }

    ret |= id;    
    }

    return ret;
}
