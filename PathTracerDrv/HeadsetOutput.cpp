#include "TracePrecomp.h"
#include "HeadsetOutput.h"
#include "Shaders.h"
#include "HeadsetSeat.h"
#include <cmath>
#include <stdexcept>

#ifdef PATHTRACER_OPENXR
// The loader is opened only when a headset is asked for, so the helper runs
// without it otherwise: every call goes through a pointer it hands out.
#define XR_USE_GRAPHICS_API_VULKAN
#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#endif

// The images an eye's picture or the HUD goes to: a swap chain of the
// runtime's, or for the simulated headset images of its own.
struct HeadsetChain
{
	uint64_t Handle = 0;   // XrSwapchain
	uint32_t Width = 0, Height = 0;
	std::vector<VkImage> Images;
	std::vector<std::unique_ptr<VulkanImage>> Owned;
	int Acquired = -1;
	int Next = 0;
	bool Released = false;   // one has been, so the runtime has a picture from it
};

struct HeadsetOutput::Impl
{
	VulkanDevice* Device = nullptr;
	VkFormat Format = VK_FORMAT_R8G8B8A8_SRGB;
	HeadsetChain Eyes[2];
	HeadsetChain Hud;

	// The encode pass: a set per frame in flight for each eye and the HUD,
	// rewritten as the frame is recorded, and an image each to encode into,
	// which is then blitted to the swap chain in whatever format it has.
	std::unique_ptr<VulkanShader> Shader;
	std::unique_ptr<VulkanDescriptorSetLayout> SetLayout;
	std::unique_ptr<VulkanDescriptorPool> Pool;
	std::unique_ptr<VulkanDescriptorSet> Sets[GpuContext::FramesInFlight][3];
	std::unique_ptr<VulkanPipelineLayout> PipelineLayout;
	std::unique_ptr<VulkanPipeline> Pipeline;
	std::unique_ptr<VulkanImage> Stage[3];
	std::unique_ptr<VulkanImageView> StageView[3];

	// Where each eye was when its picture was last drawn, which the runtime
	// is told with it so it can turn it to where the eye is when shown.
	Eye Drawn[2];
	bool EyesDrawn = false;
	uint64_t SimulatedFrame = 0;

#ifdef PATHTRACER_OPENXR
	HMODULE Loader = nullptr;
	PFN_xrGetInstanceProcAddr GetProc = nullptr;
	XrInstance Instance = XR_NULL_HANDLE;
	XrSystemId System = XR_NULL_SYSTEM_ID;
	XrSession Session = XR_NULL_HANDLE;
	XrSessionState State = XR_SESSION_STATE_UNKNOWN;
	XrSpace LocalSpace = XR_NULL_HANDLE;
	XrSpace SeatSpace = XR_NULL_HANDLE;
	XrSpace ViewSpace = XR_NULL_HANDLE;
	XrTime DisplayTime = 0;
	// The seat is put where the head is once the session begins, as soon as
	// the head can be found.
	bool RecentrePending = false;
	// The eyes have been found at least once.
	bool HeadFound = false;
	std::vector<std::string> InstanceExtensions, DeviceExtensions;

	PFN_xrDestroyInstance DestroyInstance = nullptr;
	PFN_xrGetInstanceProperties GetInstanceProperties = nullptr;
	PFN_xrGetSystem GetSystem = nullptr;
	PFN_xrGetSystemProperties GetSystemProperties = nullptr;
	PFN_xrEnumerateViewConfigurationViews EnumerateViewConfigurationViews = nullptr;
	PFN_xrGetVulkanInstanceExtensionsKHR GetVulkanInstanceExtensions = nullptr;
	PFN_xrGetVulkanDeviceExtensionsKHR GetVulkanDeviceExtensions = nullptr;
	PFN_xrGetVulkanGraphicsDeviceKHR GetVulkanGraphicsDevice = nullptr;
	PFN_xrGetVulkanGraphicsRequirementsKHR GetVulkanGraphicsRequirements = nullptr;
	PFN_xrCreateSession CreateSession = nullptr;
	PFN_xrDestroySession DestroySession = nullptr;
	PFN_xrBeginSession BeginSession = nullptr;
	PFN_xrEndSession EndSession = nullptr;
	PFN_xrPollEvent PollEvent = nullptr;
	PFN_xrWaitFrame WaitFrame = nullptr;
	PFN_xrBeginFrame BeginFrame = nullptr;
	PFN_xrEndFrame EndFrame = nullptr;
	PFN_xrLocateViews LocateViews = nullptr;
	PFN_xrLocateSpace LocateSpace = nullptr;
	PFN_xrCreateReferenceSpace CreateReferenceSpace = nullptr;
	PFN_xrDestroySpace DestroySpace = nullptr;
	PFN_xrEnumerateSwapchainFormats EnumerateSwapchainFormats = nullptr;
	PFN_xrCreateSwapchain CreateSwapchain = nullptr;
	PFN_xrDestroySwapchain DestroySwapchain = nullptr;
	PFN_xrEnumerateSwapchainImages EnumerateSwapchainImages = nullptr;
	PFN_xrAcquireSwapchainImage AcquireSwapchainImage = nullptr;
	PFN_xrWaitSwapchainImage WaitSwapchainImage = nullptr;
	PFN_xrReleaseSwapchainImage ReleaseSwapchainImage = nullptr;

	template<typename T> bool Load(T& fn, const char* name)
	{
		fn = nullptr;
		return XR_SUCCEEDED(GetProc(Instance, name, (PFN_xrVoidFunction*)&fn)) && fn;
	}
#endif
};

HeadsetOutput::HeadsetOutput(bool simulated) : I(new Impl()), Simulated(simulated)
{
}

HeadsetOutput::~HeadsetOutput()
{
	if (I->Device)
		vkDeviceWaitIdle(I->Device->device);
#ifdef PATHTRACER_OPENXR
	for (HeadsetChain* chain : { &I->Eyes[0], &I->Eyes[1], &I->Hud })
		if (chain->Handle && I->DestroySwapchain)
			I->DestroySwapchain((XrSwapchain)chain->Handle);
	if (I->ViewSpace) I->DestroySpace(I->ViewSpace);
	if (I->SeatSpace) I->DestroySpace(I->SeatSpace);
	if (I->LocalSpace) I->DestroySpace(I->LocalSpace);
	if (I->Session) I->DestroySession(I->Session);
	if (I->Instance) I->DestroyInstance(I->Instance);
	if (I->Loader) FreeLibrary(I->Loader);
#endif
}

#ifdef PATHTRACER_OPENXR
static std::vector<std::string> SplitNames(const std::string& list)
{
	std::vector<std::string> names;
	size_t start = 0;
	while (start < list.size())
	{
		size_t end = list.find(' ', start);
		if (end == std::string::npos)
			end = list.size();
		if (end > start)
			names.push_back(list.substr(start, end - start));
		start = end + 1;
	}
	return names;
}
#endif

bool HeadsetOutput::Start()
{
	if (Simulated)
	{
		RecommendedWidth = 640;
		RecommendedHeight = 704;
		StatusText = "a simulated headset, for the test harness";
		return true;
	}
#ifndef PATHTRACER_OPENXR
	StatusText = "the helper was built without the OpenXR SDK (see cmake/fetch-openxr.sh)";
	return false;
#else
	Impl& x = *I;
	x.Loader = LoadLibraryA("openxr_loader.dll");
	if (!x.Loader)
	{
		StatusText = "openxr_loader.dll is not beside the helper";
		return false;
	}
	x.GetProc = (PFN_xrGetInstanceProcAddr)GetProcAddress(x.Loader, "xrGetInstanceProcAddr");
	PFN_xrCreateInstance createInstance = nullptr;
	PFN_xrEnumerateInstanceExtensionProperties enumerateExtensions = nullptr;
	if (!x.GetProc || !x.Load(createInstance, "xrCreateInstance") || !x.Load(enumerateExtensions, "xrEnumerateInstanceExtensionProperties"))
	{
		StatusText = "openxr_loader.dll is not an OpenXR loader";
		return false;
	}

	// The runtime is found here, from the registry: SteamVR's, Meta's, or
	// whichever the player has made the active one.
	uint32_t count = 0;
	XrResult r = enumerateExtensions(nullptr, 0, &count, nullptr);
	if (XR_FAILED(r))
	{
		StatusText = "no OpenXR runtime (is SteamVR, or the headset's own software, installed and set as the OpenXR runtime?)";
		return false;
	}
	std::vector<XrExtensionProperties> extensions(count, { XR_TYPE_EXTENSION_PROPERTIES });
	enumerateExtensions(nullptr, count, &count, extensions.data());
	bool vulkan = false;
	for (const XrExtensionProperties& e : extensions)
		vulkan = vulkan || !strcmp(e.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
	if (!vulkan)
	{
		StatusText = "the OpenXR runtime does not draw with Vulkan (" XR_KHR_VULKAN_ENABLE_EXTENSION_NAME ")";
		return false;
	}

	XrInstanceCreateInfo info = { XR_TYPE_INSTANCE_CREATE_INFO };
	snprintf(info.applicationInfo.applicationName, sizeof(info.applicationInfo.applicationName), "Deus Ex, path traced");
	info.applicationInfo.applicationVersion = 1;
	snprintf(info.applicationInfo.engineName, sizeof(info.applicationInfo.engineName), "PathTracerDrv");
	info.applicationInfo.engineVersion = 1;
	info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	const char* enabled[] = { XR_KHR_VULKAN_ENABLE_EXTENSION_NAME };
	info.enabledExtensionCount = 1;
	info.enabledExtensionNames = enabled;
	r = createInstance(&info, &x.Instance);
	if (XR_FAILED(r))
	{
		StatusText = "the OpenXR runtime would not start (" + std::to_string((int)r) + ")";
		return false;
	}

	bool loaded = x.Load(x.DestroyInstance, "xrDestroyInstance") &&
		x.Load(x.GetInstanceProperties, "xrGetInstanceProperties") &&
		x.Load(x.GetSystem, "xrGetSystem") &&
		x.Load(x.GetSystemProperties, "xrGetSystemProperties") &&
		x.Load(x.EnumerateViewConfigurationViews, "xrEnumerateViewConfigurationViews") &&
		x.Load(x.GetVulkanInstanceExtensions, "xrGetVulkanInstanceExtensionsKHR") &&
		x.Load(x.GetVulkanDeviceExtensions, "xrGetVulkanDeviceExtensionsKHR") &&
		x.Load(x.GetVulkanGraphicsDevice, "xrGetVulkanGraphicsDeviceKHR") &&
		x.Load(x.GetVulkanGraphicsRequirements, "xrGetVulkanGraphicsRequirementsKHR") &&
		x.Load(x.CreateSession, "xrCreateSession") &&
		x.Load(x.DestroySession, "xrDestroySession") &&
		x.Load(x.BeginSession, "xrBeginSession") &&
		x.Load(x.EndSession, "xrEndSession") &&
		x.Load(x.PollEvent, "xrPollEvent") &&
		x.Load(x.WaitFrame, "xrWaitFrame") &&
		x.Load(x.BeginFrame, "xrBeginFrame") &&
		x.Load(x.EndFrame, "xrEndFrame") &&
		x.Load(x.LocateViews, "xrLocateViews") &&
		x.Load(x.LocateSpace, "xrLocateSpace") &&
		x.Load(x.CreateReferenceSpace, "xrCreateReferenceSpace") &&
		x.Load(x.DestroySpace, "xrDestroySpace") &&
		x.Load(x.EnumerateSwapchainFormats, "xrEnumerateSwapchainFormats") &&
		x.Load(x.CreateSwapchain, "xrCreateSwapchain") &&
		x.Load(x.DestroySwapchain, "xrDestroySwapchain") &&
		x.Load(x.EnumerateSwapchainImages, "xrEnumerateSwapchainImages") &&
		x.Load(x.AcquireSwapchainImage, "xrAcquireSwapchainImage") &&
		x.Load(x.WaitSwapchainImage, "xrWaitSwapchainImage") &&
		x.Load(x.ReleaseSwapchainImage, "xrReleaseSwapchainImage");
	if (!loaded)
	{
		StatusText = "the OpenXR runtime is missing functions it should have";
		return false;
	}

	XrInstanceProperties runtime = { XR_TYPE_INSTANCE_PROPERTIES };
	x.GetInstanceProperties(x.Instance, &runtime);

	XrSystemGetInfo system = { XR_TYPE_SYSTEM_GET_INFO };
	system.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	r = x.GetSystem(x.Instance, &system, &x.System);
	if (XR_FAILED(r))
	{
		StatusText = std::string(runtime.runtimeName) + " finds no headset (is it connected, and awake?)";
		return false;
	}
	XrSystemProperties properties = { XR_TYPE_SYSTEM_PROPERTIES };
	x.GetSystemProperties(x.Instance, x.System, &properties);

	XrViewConfigurationView views[2] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
	count = 0;
	r = x.EnumerateViewConfigurationViews(x.Instance, x.System, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count, views);
	if (XR_FAILED(r) || count != 2)
	{
		StatusText = std::string(properties.systemName) + " does not show a picture to each eye";
		return false;
	}
	RecommendedWidth = views[0].recommendedImageRectWidth;
	RecommendedHeight = views[0].recommendedImageRectHeight;

	auto names = [&](PFN_xrGetVulkanInstanceExtensionsKHR get)
	{
		uint32_t size = 0;
		get(x.Instance, x.System, 0, &size, nullptr);
		std::string list(size, '\0');
		get(x.Instance, x.System, size, &size, &list[0]);
		list.resize(strlen(list.c_str()));
		return SplitNames(list);
	};
	x.InstanceExtensions = names(x.GetVulkanInstanceExtensions);
	x.DeviceExtensions = names((PFN_xrGetVulkanInstanceExtensionsKHR)x.GetVulkanDeviceExtensions);

	// Asked for before the session, as the runtime requires, whether or not
	// the answer is used: the helper's Vulkan is what it is.
	XrGraphicsRequirementsVulkanKHR requirements = { XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR };
	x.GetVulkanGraphicsRequirements(x.Instance, x.System, &requirements);

	char status[256];
	snprintf(status, sizeof(status), "%s on %s %u.%u.%u: %ux%u an eye asked for", properties.systemName, runtime.runtimeName,
		XR_VERSION_MAJOR(runtime.runtimeVersion), XR_VERSION_MINOR(runtime.runtimeVersion), XR_VERSION_PATCH(runtime.runtimeVersion),
		RecommendedWidth, RecommendedHeight);
	StatusText = status;
	HelperLog("headset: %s", status);
	return true;
#endif
}

void HeadsetOutput::RequiredExtensions(std::vector<std::string>& instance, std::vector<std::string>& device) const
{
#ifdef PATHTRACER_OPENXR
	instance.insert(instance.end(), I->InstanceExtensions.begin(), I->InstanceExtensions.end());
	device.insert(device.end(), I->DeviceExtensions.begin(), I->DeviceExtensions.end());
#endif
}

bool HeadsetOutput::UsesDevice(VkInstance instance, VkPhysicalDevice physicalDevice)
{
	if (Simulated)
		return true;
#ifdef PATHTRACER_OPENXR
	VkPhysicalDevice wanted = VK_NULL_HANDLE;
	if (XR_FAILED(I->GetVulkanGraphicsDevice(I->Instance, I->System, instance, &wanted)) || wanted != physicalDevice)
	{
		StatusText = "the headset is driven by another GPU than the game's";
		return false;
	}
	return true;
#else
	return false;
#endif
}

bool HeadsetOutput::CreateSession(VulkanDevice* device)
{
	Impl& x = *I;
	x.Device = device;

	// The encode pass: the picture, gamma encoded, into linear light at its
	// own size in an image of its own.
	x.Shader = ShaderBuilder()
		.Type(ShaderType::Compute)
		.AddSource("shaders/HeadsetEncode.comp", Shaders::HeadsetEncode())
		.DebugName("PathTracerHeadsetEncode")
		.Create("PathTracerHeadsetEncode", device);
	x.SetLayout = DescriptorSetLayoutBuilder()
		.AddBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.DebugName("PathTracerHeadsetSetLayout")
		.Create(device);
	x.Pool = DescriptorPoolBuilder()
		.AddPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * 3 * GpuContext::FramesInFlight)
		.MaxSets(3 * GpuContext::FramesInFlight)
		.DebugName("PathTracerHeadsetPool")
		.Create(device);
	for (auto& sets : x.Sets)
		for (auto& set : sets)
			set = x.Pool->allocate(x.SetLayout.get());
	x.PipelineLayout = PipelineLayoutBuilder()
		.AddSetLayout(x.SetLayout.get())
		.AddPushConstantRange(VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(float) * 4 + sizeof(int32_t) * 4)
		.DebugName("PathTracerHeadsetPipelineLayout")
		.Create(device);
	x.Pipeline = ComputePipelineBuilder()
		.Layout(x.PipelineLayout.get())
		.ComputeShader(x.Shader.get())
		.DebugName("PathTracerHeadsetPipeline")
		.Create(device);

	if (Simulated)
	{
		SessionMade = true;
		Running = true;
		return true;
	}

#ifdef PATHTRACER_OPENXR
	XrGraphicsBindingVulkanKHR binding = { XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR };
	binding.instance = device->Instance->Instance;
	binding.physicalDevice = device->PhysicalDevice.Device;
	binding.device = device->device;
	binding.queueFamilyIndex = (uint32_t)device->GraphicsFamily;
	binding.queueIndex = 0;
	XrSessionCreateInfo info = { XR_TYPE_SESSION_CREATE_INFO };
	info.next = &binding;
	info.systemId = x.System;
	XrResult r = x.CreateSession(x.Instance, &info, &x.Session);
	if (XR_FAILED(r))
	{
		StatusText = "the OpenXR runtime would not open a session (" + std::to_string((int)r) + ")";
		x.Session = XR_NULL_HANDLE;
		return false;
	}
	Session = (uint64_t)x.Session;

	XrReferenceSpaceCreateInfo space = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	space.poseInReferenceSpace.orientation.w = 1.0f;
	space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	x.CreateReferenceSpace(x.Session, &space, &x.LocalSpace);
	x.CreateReferenceSpace(x.Session, &space, &x.SeatSpace);
	space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	x.CreateReferenceSpace(x.Session, &space, &x.ViewSpace);

	// Eight bit sRGB where it is offered, which the runtime decodes itself;
	// the pictures are blitted in, so any format that takes a blit serves.
	uint32_t count = 0;
	x.EnumerateSwapchainFormats(x.Session, 0, &count, nullptr);
	std::vector<int64_t> formats(count);
	x.EnumerateSwapchainFormats(x.Session, count, &count, formats.data());
	const VkFormat preferred[] = { VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R16G16B16A16_SFLOAT };
	x.Format = VK_FORMAT_UNDEFINED;
	for (VkFormat f : preferred)
		for (int64_t offered : formats)
			if (x.Format == VK_FORMAT_UNDEFINED && offered == (int64_t)f)
				x.Format = f;
	if (x.Format == VK_FORMAT_UNDEFINED && !formats.empty())
		x.Format = (VkFormat)formats[0];
	VkFormatProperties formatProperties = {};
	vkGetPhysicalDeviceFormatProperties(device->PhysicalDevice.Device, x.Format, &formatProperties);
	if (!(formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT))
	{
		StatusText = "the headset's swap chains take no format the picture can be copied into";
		return false;
	}
	HelperLog("headset: session open, swap chains in format %d", (int)x.Format);
	return true;
#else
	return false;
#endif
}

void HeadsetOutput::Fail(const std::string& why)
{
	HelperLog("headset: %s; the screen carries on alone", why.c_str());
	StatusText = why;
	Running = false;
	Begun = false;
#ifdef PATHTRACER_OPENXR
	if (I->Device)
		vkDeviceWaitIdle(I->Device->device);
	for (HeadsetChain* chain : { &I->Eyes[0], &I->Eyes[1], &I->Hud })
	{
		if (chain->Handle && I->DestroySwapchain)
			I->DestroySwapchain((XrSwapchain)chain->Handle);
		*chain = HeadsetChain();
	}
	if (I->Session)
	{
		I->DestroySession(I->Session);
		I->Session = XR_NULL_HANDLE;
		I->LocalSpace = I->SeatSpace = I->ViewSpace = XR_NULL_HANDLE;
	}
	Session = 0;
#endif
	SessionMade = false;
}

bool HeadsetOutput::BeginFrame(bool recenter)
{
	Begun = false;
	ShouldRender = false;
	if (Simulated)
	{
		if (!SessionMade)
			return false;
		// Two eyes 64 mm apart, the head turning slowly one way and back,
		// each seeing further out than in, as a real headset's do.
		const float yaw = 0.15f * std::sin((float)I->SimulatedFrame++ * 0.1f);
		for (int e = 0; e < 2; e++)
		{
			Eye& eye = Eyes[e];
			eye.Orientation[0] = 0.0f;
			eye.Orientation[1] = std::sin(yaw * 0.5f);
			eye.Orientation[2] = 0.0f;
			eye.Orientation[3] = std::cos(yaw * 0.5f);
			eye.Position = eye.Turn(vec3(e ? 0.032f : -0.032f, 0.0f, 0.0f));
			const float outward = std::tan(0.87f), inward = std::tan(0.75f);
			eye.Left = e ? -inward : -outward;
			eye.Right = e ? outward : inward;
			eye.Up = std::tan(0.73f);
			eye.Down = -std::tan(0.84f);
		}
		I->HeadFound = true;
		Begun = true;
		ShouldRender = true;
		return true;
	}

#ifdef PATHTRACER_OPENXR
	Impl& x = *I;
	if (!x.Session)
		return false;

	// The runtime's news: the session to begin once the headset is ready to
	// show it, to end when it stops, and to give up when it is lost.
	XrEventDataBuffer event = { XR_TYPE_EVENT_DATA_BUFFER };
	while (x.PollEvent(x.Instance, &event) == XR_SUCCESS)
	{
		if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
		{
			const auto& changed = *(const XrEventDataSessionStateChanged*)&event;
			x.State = changed.state;
			HelperLog("headset: session state %d", (int)x.State);
			if (x.State == XR_SESSION_STATE_READY)
			{
				XrSessionBeginInfo begin = { XR_TYPE_SESSION_BEGIN_INFO };
				begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				const XrResult r = x.BeginSession(x.Session, &begin);
				Running = XR_SUCCEEDED(r);
				x.RecentrePending = Running;
				x.DisplayTime = 0;
				if (!Running)
					HelperLog("headset: the session would not begin (%d)", (int)r);
			}
			else if (x.State == XR_SESSION_STATE_STOPPING)
			{
				x.EndSession(x.Session);
				Running = false;
			}
			else if (x.State == XR_SESSION_STATE_EXITING || x.State == XR_SESSION_STATE_LOSS_PENDING)
			{
				Fail(x.State == XR_SESSION_STATE_EXITING ? "the OpenXR runtime closed the session" : "the headset was lost");
				return false;
			}
		}
		else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
		{
			Fail("the OpenXR runtime is going away");
			return false;
		}
		event = { XR_TYPE_EVENT_DATA_BUFFER };
	}
	if (!Running)
		return false;

	// Wherever the head is now becomes straight ahead: the seat's space moved
	// to it, turned only about the vertical so the horizon stays level. When
	// asked, and as the session begins: a runtime's own origin may be at the
	// floor, or where the head was when the headset was last set up standing,
	// and the eyes are drawn from where the head is from the seat's middle,
	// so anything but the head there puts them above or below the player's.
	if ((recenter || x.RecentrePending) && x.DisplayTime)
	{
		XrSpaceLocation head = { XR_TYPE_SPACE_LOCATION };
		if (XR_SUCCEEDED(x.LocateSpace(x.ViewSpace, x.LocalSpace, x.DisplayTime, &head)) &&
			(head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) && (head.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
		{
			Eye current;
			current.Orientation[0] = head.pose.orientation.x;
			current.Orientation[1] = head.pose.orientation.y;
			current.Orientation[2] = head.pose.orientation.z;
			current.Orientation[3] = head.pose.orientation.w;
			const vec3 ahead = current.Turn(vec3(0.0f, 0.0f, -1.0f));
			const float yaw = std::atan2(-ahead.x, -ahead.z);
			XrReferenceSpaceCreateInfo space = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
			space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
			space.poseInReferenceSpace.orientation = { 0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f) };
			space.poseInReferenceSpace.position = head.pose.position;
			XrSpace seat = XR_NULL_HANDLE;
			if (XR_SUCCEEDED(x.CreateReferenceSpace(x.Session, &space, &seat)))
			{
				x.DestroySpace(x.SeatSpace);
				x.SeatSpace = seat;
				x.RecentrePending = false;
				HelperLog("headset: recentred, the head %.2f m right, %.2f m up and %.2f m back from the runtime's origin, turned %.0f degrees",
					head.pose.position.x, head.pose.position.y, head.pose.position.z, yaw * 57.2958f);
			}
		}
	}

	XrFrameWaitInfo wait = { XR_TYPE_FRAME_WAIT_INFO };
	XrFrameState state = { XR_TYPE_FRAME_STATE };
	XrResult r = x.WaitFrame(x.Session, &wait, &state);
	if (XR_FAILED(r))
	{
		Fail("the OpenXR runtime would not give a frame (" + std::to_string((int)r) + ")");
		return false;
	}
	XrFrameBeginInfo begin = { XR_TYPE_FRAME_BEGIN_INFO };
	r = x.BeginFrame(x.Session, &begin);
	if (XR_FAILED(r))
	{
		Fail("the OpenXR runtime would not begin a frame (" + std::to_string((int)r) + ")");
		return false;
	}
	Begun = true;
	x.DisplayTime = state.predictedDisplayTime;
	ShouldRender = state.shouldRender == XR_TRUE;

	if (ShouldRender)
	{
		XrViewLocateInfo locate = { XR_TYPE_VIEW_LOCATE_INFO };
		locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		locate.displayTime = state.predictedDisplayTime;
		locate.space = x.SeatSpace;
		XrViewState viewState = { XR_TYPE_VIEW_STATE };
		XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
		uint32_t count = 0;
		r = x.LocateViews(x.Session, &locate, &viewState, 2, &count, views);
		// Where the eyes cannot be found - the headset lost its tracking -
		// there is nothing to draw them from: the last frame stays up.
		if (XR_FAILED(r) || count != 2 || !(viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
			ShouldRender = false;
		else
		{
			x.HeadFound = true;
			for (int e = 0; e < 2; e++)
			{
				Eye& eye = Eyes[e];
				const XrPosef& pose = views[e].pose;
				eye.Orientation[0] = pose.orientation.x;
				eye.Orientation[1] = pose.orientation.y;
				eye.Orientation[2] = pose.orientation.z;
				eye.Orientation[3] = pose.orientation.w;
				if (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)
					eye.Position = vec3(pose.position.x, pose.position.y, pose.position.z);
				eye.Left = std::tan(views[e].fov.angleLeft);
				eye.Right = std::tan(views[e].fov.angleRight);
				eye.Up = std::tan(views[e].fov.angleUp);
				eye.Down = std::tan(views[e].fov.angleDown);
			}
		}
	}
	return true;
#else
	return false;
#endif
}

// A chain of images at a size, made again when the size changes. The caller
// has waited for the frames in flight first, which may still be writing to
// the old one.
static bool EnsureChain(HeadsetOutput::Impl& x, HeadsetChain& chain, uint32_t width, uint32_t height, bool simulated)
{
	if (chain.Images.size() && chain.Width == width && chain.Height == height)
		return true;
#ifdef PATHTRACER_OPENXR
	if (chain.Handle)
		x.DestroySwapchain((XrSwapchain)chain.Handle);
#endif
	chain = HeadsetChain();
	chain.Width = width;
	chain.Height = height;
	if (simulated)
	{
		for (int i = 0; i < 3; i++)
		{
			chain.Owned.push_back(ImageBuilder()
				.Format(x.Format)
				.Size((int)width, (int)height)
				.Usage(VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
				.DebugName("PathTracerHeadsetSimulated")
				.Create(x.Device));
			chain.Images.push_back(chain.Owned.back()->image);
		}
		return true;
	}
#ifdef PATHTRACER_OPENXR
	XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	info.format = (int64_t)x.Format;
	info.sampleCount = 1;
	info.width = width;
	info.height = height;
	info.faceCount = 1;
	info.arraySize = 1;
	info.mipCount = 1;
	XrSwapchain handle = XR_NULL_HANDLE;
	XrResult r = x.CreateSwapchain(x.Session, &info, &handle);
	if (XR_FAILED(r))
	{
		HelperLog("headset: no swap chain at %ux%u (%d)", width, height, (int)r);
		return false;
	}
	chain.Handle = (uint64_t)handle;
	uint32_t count = 0;
	x.EnumerateSwapchainImages(handle, 0, &count, nullptr);
	std::vector<XrSwapchainImageVulkanKHR> images(count, { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR });
	x.EnumerateSwapchainImages(handle, count, &count, (XrSwapchainImageBaseHeader*)images.data());
	for (const XrSwapchainImageVulkanKHR& image : images)
		chain.Images.push_back(image.image);
	HelperLog("headset: swap chain %ux%u, %u images", width, height, count);
	return count > 0;
#else
	return false;
#endif
}

// The next image of the chain to draw into, waited for until the runtime is
// done showing it.
static VkImage AcquireImage(HeadsetOutput::Impl& x, HeadsetChain& chain, bool simulated)
{
	uint32_t index = 0;
	if (simulated)
	{
		index = (uint32_t)chain.Next;
		chain.Next = (chain.Next + 1) % (int)chain.Images.size();
	}
	else
	{
#ifdef PATHTRACER_OPENXR
		XrSwapchainImageAcquireInfo acquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		if (XR_FAILED(x.AcquireSwapchainImage((XrSwapchain)chain.Handle, &acquire, &index)))
			return VK_NULL_HANDLE;
		XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
		wait.timeout = XR_INFINITE_DURATION;
		if (XR_FAILED(x.WaitSwapchainImage((XrSwapchain)chain.Handle, &wait)))
		{
			XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
			x.ReleaseSwapchainImage((XrSwapchain)chain.Handle, &release);
			return VK_NULL_HANDLE;
		}
#else
		return VK_NULL_HANDLE;
#endif
	}
	chain.Acquired = (int)index;
	return chain.Images[index];
}

// One picture: encoded into the stage image, then blitted into the chain's
// image, which the blit converts to its format. The image is written whole,
// so whatever layout it came in is taken as undefined; it goes back in
// COLOR_ATTACHMENT_OPTIMAL, as a runtime's has to.
static void RecordPicture(HeadsetOutput::Impl& x, VulkanCommandBuffer* commands, int which, VulkanImageView* source,
	uint32_t width, uint32_t height, float gamma, bool hud, int slot, VkImage target)
{
	if (!x.Stage[which] || x.Stage[which]->width != (int)width || x.Stage[which]->height != (int)height)
	{
		x.Stage[which] = ImageBuilder()
			.Format(VK_FORMAT_R16G16B16A16_SFLOAT)
			.Size((int)width, (int)height)
			.Usage(VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
			.DebugName("PathTracerHeadsetStage")
			.Create(x.Device);
		x.StageView[which] = ImageViewBuilder().Image(x.Stage[which].get(), VK_FORMAT_R16G16B16A16_SFLOAT).DebugName("PathTracerHeadsetStage").Create(x.Device);
	}
	VulkanDescriptorSet* set = x.Sets[slot][which].get();
	WriteDescriptors()
		.AddStorageImage(set, 0, source, VK_IMAGE_LAYOUT_GENERAL)
		.AddStorageImage(set, 1, x.StageView[which].get(), VK_IMAGE_LAYOUT_GENERAL)
		.Execute(x.Device);

	PipelineBarrier()
		.AddImage(x.Stage[which].get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT)
		.Execute(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	struct { float Params[4]; int32_t Size[4]; } constants = { { gamma, hud ? 1.0f : 0.0f, 0.0f, 0.0f }, { (int32_t)width, (int32_t)height, 0, 0 } };
	commands->bindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, x.Pipeline.get());
	commands->bindDescriptorSet(VK_PIPELINE_BIND_POINT_COMPUTE, x.PipelineLayout.get(), 0, set);
	commands->pushConstants(x.PipelineLayout.get(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
	commands->dispatch((width + 7) / 8, (height + 7) / 8, 1);

	PipelineBarrier()
		.AddImage(x.Stage[which].get(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT)
		.AddImage(target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT)
		.Execute(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	VkImageBlit blit = {};
	blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	blit.srcOffsets[1] = { (int32_t)width, (int32_t)height, 1 };
	blit.dstSubresource = blit.srcSubresource;
	blit.dstOffsets[1] = blit.srcOffsets[1];
	commands->blitImage(x.Stage[which]->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
	PipelineBarrier()
		.AddImage(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT)
		.Execute(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
}

void HeadsetOutput::RecordEyes(VulkanCommandBuffer* commands, VulkanImageView* left, VulkanImageView* right, uint32_t width, uint32_t height, float gamma, int slot)
{
	Impl& x = *I;
	if (!WantsPictures() || !left || !right)
		return;
	VulkanImageView* sources[2] = { left, right };
	for (int e = 0; e < 2; e++)
	{
		HeadsetChain& chain = x.Eyes[e];
		if (!EnsureChain(x, chain, width, height, Simulated))
		{
			Fail("the headset would not take pictures of " + std::to_string(width) + "x" + std::to_string(height));
			return;
		}
		const VkImage target = AcquireImage(x, chain, Simulated);
		if (!target)
		{
			Fail("the headset gave no image to draw an eye into");
			return;
		}
		RecordPicture(x, commands, e, sources[e], width, height, gamma, false, slot, target);
		x.Drawn[e] = Eyes[e];
	}
	x.EyesDrawn = true;
}

void HeadsetOutput::RecordHud(VulkanCommandBuffer* commands, VulkanImageView* hud, uint32_t width, uint32_t height, float gamma, int slot)
{
	Impl& x = *I;
	if (!Begun || !hud || !width || !height)
		return;
	if (!EnsureChain(x, x.Hud, width, height, Simulated))
		return;
	const VkImage target = AcquireImage(x, x.Hud, Simulated);
	if (!target)
		return;
	RecordPicture(x, commands, 2, hud, width, height, gamma, true, slot, target);
}

// The simulated headset's images, once, to files beside the helper - what a
// real headset would have been handed - for the test harness's runner to
// look at: each eye, and the HUD's colour over grey and its coverage.
static void WriteSimulated(HeadsetOutput::Impl& x)
{
	vkDeviceWaitIdle(x.Device->device);
	auto pool = CommandPoolBuilder().QueueFamily(x.Device->GraphicsFamily).Create(x.Device);
	auto write = [&](HeadsetChain& chain, const char* name, bool hud)
	{
		if (chain.Images.empty() || !chain.Released)
			return;
		const int index = (chain.Next + (int)chain.Images.size() - 1) % (int)chain.Images.size();
		const VkImage image = chain.Images[index];
		const size_t bytes = (size_t)chain.Width * chain.Height * 4;
		auto buffer = BufferBuilder().Size(bytes).Usage(VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU).Create(x.Device);
		auto commands = pool->createBuffer();
		commands->begin();
		PipelineBarrier()
			.AddImage(image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT)
			.Execute(commands.get(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		VkBufferImageCopy region = {};
		region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		region.imageExtent = { chain.Width, chain.Height, 1 };
		vkCmdCopyImageToBuffer(commands->buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer->buffer, 1, &region);
		PipelineBarrier()
			.AddImage(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT)
			.Execute(commands.get(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
		commands->end();
		auto fence = FenceBuilder().Create(x.Device);
		QueueSubmit().AddCommandBuffer(commands.get()).Execute(x.Device, x.Device->GraphicsQueue, fence.get());
		vkWaitForFences(x.Device->device, 1, &fence->fence, VK_TRUE, UINT64_MAX);
		const uint8_t* texels = (const uint8_t*)buffer->Map(0, bytes);
		FILE* f = fopen(name, "wb");
		if (f)
		{
			fprintf(f, "P6\n%u %u\n255\n", hud ? chain.Width * 2 : chain.Width, chain.Height);
			for (uint32_t y = 0; y < chain.Height; y++)
			{
				const uint8_t* row = texels + (size_t)y * chain.Width * 4;
				for (uint32_t i = 0; i < chain.Width; i++)
				{
					// The colour, which is premultiplied, over mid grey.
					const uint8_t* t = row + i * 4;
					for (int c = 0; c < 3; c++)
						fputc(hud ? std::min(255, t[c] + (255 - t[3]) * 128 / 255) : t[c], f);
				}
				if (hud)
					for (uint32_t i = 0; i < chain.Width; i++)
						for (int c = 0; c < 3; c++)
							fputc(row[i * 4 + 3], f);
			}
			fclose(f);
		}
		buffer->Unmap();
	};
	write(x.Eyes[0], "headset-left.ppm", false);
	write(x.Eyes[1], "headset-right.ppm", false);
	write(x.Hud, "headset-hud.ppm", true);
	HelperLog("headset: the simulated headset's pictures written");
}

bool HeadsetOutput::HeadOrientation(float* q) const
{
	if (!I->HeadFound)
		return false;
	for (int i = 0; i < 4; i++)
		q[i] = Eyes[0].Orientation[i];
	return true;
}

void HeadsetOutput::EndFrame(const HudPanel* hudPanels, int panelCount)
{
	if (!Begun)
		return;
	Begun = false;
	Impl& x = *I;
	for (HeadsetChain* chain : { &x.Eyes[0], &x.Eyes[1], &x.Hud })
	{
		if (chain->Acquired < 0)
			continue;
#ifdef PATHTRACER_OPENXR
		if (!Simulated)
		{
			XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
			x.ReleaseSwapchainImage((XrSwapchain)chain->Handle, &release);
		}
#endif
		chain->Acquired = -1;
		chain->Released = true;
	}
	if (Simulated)
	{
		if (x.SimulatedFrame == 40)
			WriteSimulated(x);
		return;
	}

#ifdef PATHTRACER_OPENXR
	// The eyes' last pictures, where they were drawn from: this frame's, or
	// for a frame with nothing traced the last, which the runtime turns to
	// where the head is now.
	XrCompositionLayerProjectionView views[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
	for (int e = 0; e < 2; e++)
	{
		const Eye& eye = x.Drawn[e];
		views[e].pose.orientation = { eye.Orientation[0], eye.Orientation[1], eye.Orientation[2], eye.Orientation[3] };
		views[e].pose.position = { eye.Position.x, eye.Position.y, eye.Position.z };
		views[e].fov.angleLeft = std::atan(eye.Left);
		views[e].fov.angleRight = std::atan(eye.Right);
		views[e].fov.angleUp = std::atan(eye.Up);
		views[e].fov.angleDown = std::atan(eye.Down);
		views[e].subImage.swapchain = (XrSwapchain)x.Eyes[e].Handle;
		views[e].subImage.imageRect.extent = { (int32_t)x.Eyes[e].Width, (int32_t)x.Eyes[e].Height };
	}
	XrCompositionLayerProjection projection = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	projection.space = x.SeatSpace;
	projection.viewCount = 2;
	projection.views = views;

	// The HUD's panels, as far out as asked along the way each faces.
	XrCompositionLayerQuad panels[MaxHudPanels];
	const XrCompositionLayerBaseHeader* layers[1 + MaxHudPanels];
	uint32_t layerCount = 0;
	if (ShouldRender && x.EyesDrawn && x.Eyes[0].Released && x.Eyes[1].Released)
		layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&projection;
	for (int p = 0; p < panelCount && p < MaxHudPanels; p++)
	{
		const HudPanel& from = hudPanels[p];
		const float distance = from.Distance;
		if (!ShouldRender || !x.Hud.Released || distance <= 0.0f || from.TangentX <= 0.0f || from.TangentY <= 0.0f || from.Width <= 0 || from.Height <= 0)
			continue;
		const vec3 along = HeadsetSeat::Turn(from.Orientation, vec3(0.0f, 0.0f, -distance));
		XrCompositionLayerQuad& panel = panels[p];
		panel = { XR_TYPE_COMPOSITION_LAYER_QUAD };
		panel.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		panel.space = x.SeatSpace;
		panel.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		panel.subImage.swapchain = (XrSwapchain)x.Hud.Handle;
		panel.subImage.imageRect.offset = { from.X, from.Y };
		panel.subImage.imageRect.extent = { from.Width, from.Height };
		panel.pose.orientation = { from.Orientation[0], from.Orientation[1], from.Orientation[2], from.Orientation[3] };
		panel.pose.position = { along.x, along.y, along.z };
		panel.size = { 2.0f * distance * from.TangentX, 2.0f * distance * from.TangentY };
		layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&panel;
	}

	XrFrameEndInfo end = { XR_TYPE_FRAME_END_INFO };
	end.displayTime = x.DisplayTime;
	end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	end.layerCount = layerCount;
	end.layers = layers;
	const XrResult r = x.EndFrame(x.Session, &end);
	if (XR_FAILED(r))
		Fail("the OpenXR runtime would not end a frame (" + std::to_string((int)r) + ")");
#endif
}
