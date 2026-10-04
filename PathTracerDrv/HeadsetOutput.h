#pragma once

#include "vec.h"
#include "GpuContext.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class VulkanDevice;
class VulkanCommandBuffer;
class VulkanImageView;

// A headset the helper's frames are shown on, through OpenXR: a picture for
// each eye, traced from where that eye is, and the HUD on a panel in front of
// them.
//
// The room the headset tracks in is fitted to the view the device describes:
// the player's eyes where the seat is, straight ahead the way the player
// faces, but level - the mouse turns the player, and the room turns with
// them, while looking up and down with it leaves the horizon where it is.
// The head looks round inside the room and leans, and the HUD's panel spans
// the view the device describes wherever it points, so what the HUD marks
// lines up with the world behind it and the crosshair is where the shot
// goes.
//
// The runtime sets the pace: BeginFrame waits for the moment to start the
// next frame, which holds the helper - and the game waiting on it - to the
// headset's refresh. Where a frame takes longer, the runtime turns the last
// one as the head turns until the next arrives; a menu over no level is shown
// that way too, the last picture turned as the head turns, under the HUD.
//
// For the test harness a simulated one stands in: no runtime, two eyes
// looking a little one way and the other, and their pictures kept rather than
// shown. It is only ever asked for by PathTracerHelperTest.
class HeadsetOutput
{
public:
	explicit HeadsetOutput(bool simulated);
	~HeadsetOutput();

	// Before the Vulkan instance: the runtime, the headset, and what they need
	// of Vulkan. False when there is none to show on, and Status says why.
	bool Start();
	void RequiredExtensions(std::vector<std::string>& instance, std::vector<std::string>& device) const;
	// Once the instance exists: whether the runtime draws on this GPU, which
	// is the game's. The picture cannot go anywhere else.
	bool UsesDevice(VkInstance instance, VkPhysicalDevice physicalDevice);
	// Once the device exists: the session, and the pass the pictures are
	// encoded for it with.
	bool CreateSession(VulkanDevice* device);
	// Something went wrong it cannot carry on from: the session goes, and the
	// helper carries on for the screen alone.
	void Fail(const std::string& why);

	// A session exists, and whether the runtime is showing it: it is not
	// while the headset sleeps, or is off the head.
	bool Available() const { return Session != 0 || (Simulated && SessionMade); }
	bool Showing() const { return Running; }
	bool IsSimulated() const { return Simulated; }
	const char* Status() const { return StatusText.c_str(); }
	// The size of an eye's picture the runtime asks for.
	uint32_t EyeWidth() const { return RecommendedWidth; }
	uint32_t EyeHeight() const { return RecommendedHeight; }

	// An eye for the frame begun: where it is in the seat's space, in metres
	// - x right, y up, z back, as OpenXR has it - which way it faces, as a
	// quaternion, and the tangents of its view's edges, left and down
	// negative.
	struct Eye
	{
		float Orientation[4] = { 0.0f, 0.0f, 0.0f, 1.0f };   // x, y, z, w
		vec3 Position = vec3(0.0f, 0.0f, 0.0f);
		float Left = -1.0f, Right = 1.0f, Up = 1.0f, Down = -1.0f;

		// A direction in the eye's own terms turned into the seat's.
		vec3 Turn(const vec3& v) const
		{
			const vec3 u(Orientation[0], Orientation[1], Orientation[2]);
			const vec3 uv = cross(u, v);
			return v + (uv * Orientation[3] + cross(u, uv)) * 2.0f;
		}
	};

	// Each frame, before it is traced: the runtime's news, and while it is
	// showing, a wait for the moment to start the frame and where the eyes
	// will be when it is shown. True when a frame is begun, which EndFrame
	// must then end. recenter makes wherever the head is now straight ahead.
	bool BeginFrame(bool recenter);
	// The frame begun wants pictures: the runtime can see them.
	bool WantsPictures() const { return Begun && ShouldRender; }
	const Eye& GetEye(int eye) const { return Eyes[eye]; }

	// Recorded into the frame's hand over, once the pictures are done, both
	// gamma encoded as the screen takes them: the eyes', GENERAL, their size
	// the swap chains' (made again when it changes); and the HUD,
	// premultiplied, over the view the device describes. Each is encoded for
	// its swap chain, with the Brightness's exponent on it, and copied to an
	// image acquired from it. slot is the frame in flight it is recorded for.
	void RecordEyes(VulkanCommandBuffer* commands, VulkanImageView* left, VulkanImageView* right, uint32_t width, uint32_t height, float gamma, int slot);
	void RecordHud(VulkanCommandBuffer* commands, VulkanImageView* hud, uint32_t width, uint32_t height, float gamma, int slot);

	// A panel the HUD is shown on: a part of the HUD's image, turned in the
	// seat's space as orientation (a quaternion, x, y, z, w) says and
	// distance metres out along it, as wide and tall as the tangents of its
	// half extents say at that distance.
	struct HudPanel
	{
		float Orientation[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		int32_t X = 0, Y = 0, Width = 0, Height = 0;
		float TangentX = 0.0f, TangentY = 0.0f;
		float Distance = 0.0f;
	};
	static const int MaxHudPanels = 3;

	// Once the hand over is submitted: the images released, and the frame
	// ended with the eyes' pictures - this frame's, or the last, where they
	// were drawn from, which the runtime turns to where the head is now -
	// and the HUD's panels, the last over the first.
	void EndFrame(const HudPanel* panels, int panelCount);

	// Which way the head faces in the seat's space, as of the last frame the
	// eyes were found in. False before there has been one.
	bool HeadOrientation(float* q) const;

	// The runtime's handles and the pass's objects, which only
	// HeadsetOutput.cpp sees inside.
	struct Impl;

private:
	std::unique_ptr<Impl> I;

	bool Simulated = false;
	bool SessionMade = false;
	bool Running = false;
	bool Begun = false;
	bool ShouldRender = false;
	std::string StatusText = "not started";
	uint32_t RecommendedWidth = 0, RecommendedHeight = 0;
	Eye Eyes[2];
	uint64_t Session = 0;
};
