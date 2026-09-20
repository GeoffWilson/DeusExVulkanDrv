
#include "Precomp.h"
#include "VRBackend.h"

// Stands in for OpenXRBackend.cpp where the OpenXR headers are not available.
//
// They are not bundled with this repository, and the Visual Studio project does
// not list OpenXRBackend.cpp either, so a build from a clean checkout has no
// OpenXR to compile against. CreateOpenXRBackend already documents nullptr as
// meaning "no loader or runtime present", and the caller handles that by
// staying mono - so this is the contract the real backend has on a machine
// without a headset, not a special case.
//
// Deus Ex has no VR support to drive from here in any event; UseVR is a UT
// feature and the README says it is not one that ports across.
VRBackend* CreateOpenXRBackend()
{
	return nullptr;
}
