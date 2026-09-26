// PathTracerHelperTest.exe: drives PathTracerHelper.exe with a made up scene,
// the way the render device does, and writes what comes back to a file.
//
// 32-bit, like the game, so the whole road is the one the game takes: a
// 32-bit Vulkan device here, the helper on the same GPU, the scene over the
// shared memory, and each frame taken over through the shared image and
// semaphores. What it cannot check is the engine's end - LevelScene and the
// textures - which only the game can.
//
//   PathTracerHelperTest.exe [frames] [width] [height]    (helper beside it)
//
// Halfway through, the size goes up by half, as a change of resolution does,
// so the helper makes a new shared image and this side takes it up.
//
// Writes helper-test.ppm: a floor with a glossy checkerboard, a red box and a
// wall, lit by one light. Exits 0 when every frame arrived and the picture is
// lit.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <zvulkan/vulkandevice.h>
#include <zvulkan/vulkaninstance.h>
#include <zvulkan/vulkanbuilders.h>
#include <zvulkan/vulkancompatibledevice.h>
#include "TraceClient.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

void VulkanPrintLog(const char* typestr, const std::string& msg)
{
	printf("[%s] %s\n", typestr, msg.c_str());
}

void VulkanError(const char* text)
{
	throw std::runtime_error(text);
}

static float HalfToFloat(uint16_t h)
{
	const uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 31, mantissa = h & 1023;
	float value;
	if (exponent == 0)
		value = std::ldexp((float)mantissa, -24);
	else if (exponent == 31)
		value = mantissa ? NAN : INFINITY;
	else
		value = std::ldexp((float)(mantissa | 1024), (int)exponent - 25);
	return sign ? -value : value;
}

static vec3 Cross(const vec3& a, const vec3& b) { return vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static vec3 Normalized(const vec3& v) { float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); return vec3(v.x / l, v.y / l, v.z / l); }

// A quad as two triangles, textured or not, into the geometry.
static void AddQuad(SceneGeometry& g, vec3 a, vec3 b, vec3 c, vec3 d, vec3 albedo, int texture, float uvScale)
{
	const vec3 normal = Normalized(Cross(vec3(b.x - a.x, b.y - a.y, b.z - a.z), vec3(c.x - a.x, c.y - a.y, c.z - a.z)));
	const vec3 corners[2][3] = { { a, b, c }, { a, c, d } };
	const float uvs[2][6] = { { 0, 0, uvScale, 0, uvScale, uvScale }, { 0, 0, uvScale, uvScale, 0, uvScale } };
	for (int t = 0; t < 2; t++)
	{
		for (int v = 0; v < 3; v++)
			g.Positions.push_back(corners[t][v]);
		TriangleAttributes attr = {};
		attr.Normal = vec4(normal.x, normal.y, normal.z, 0.0f);
		attr.Albedo = vec4(albedo.x, albedo.y, albedo.z, 0.0f);
		attr.Emission = vec4(0.0f, 0.0f, 0.0f, 0.0f);
		attr.Ambient = vec4(0.03f, 0.03f, 0.04f, 0.0f);
		attr.UV01 = vec4(uvs[t][0], uvs[t][1], uvs[t][2], uvs[t][3]);
		attr.UV2Tex = vec4(uvs[t][4], uvs[t][5], (float)texture, 0.0f);
		g.Attributes.push_back(attr);
	}
}

static void AddBox(SceneGeometry& g, vec3 lo, vec3 hi, vec3 albedo)
{
	vec3 p[8];
	for (int i = 0; i < 8; i++)
		p[i] = vec3((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z);
	AddQuad(g, p[4], p[5], p[7], p[6], albedo, -1, 1);   // top
	AddQuad(g, p[0], p[2], p[3], p[1], albedo, -1, 1);   // bottom
	AddQuad(g, p[0], p[1], p[5], p[4], albedo, -1, 1);   // -y
	AddQuad(g, p[2], p[6], p[7], p[3], albedo, -1, 1);   // +y
	AddQuad(g, p[0], p[4], p[6], p[2], albedo, -1, 1);   // -x
	AddQuad(g, p[1], p[3], p[7], p[5], albedo, -1, 1);   // +x
}

int main(int argc, char** argv)
{
	const int frames = argc > 1 ? atoi(argv[1]) : 30;
	const uint32_t width = argc > 2 ? (uint32_t)atoi(argv[2]) : 320;
	const uint32_t height = argc > 3 ? (uint32_t)atoi(argv[3]) : 240;
	setvbuf(stdout, nullptr, _IONBF, 0);
	printf("pointer size: %d bits\n", (int)(sizeof(void*) * 8));

	try
	{
		auto instance = VulkanInstanceBuilder().Create();
		VulkanDeviceBuilder builder;
		builder.RequireExtension(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
		builder.RequireExtension(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
		auto device = builder.Create(instance);
		printf("this side: %s\n", device->PhysicalDevice.Properties.Properties.deviceName);

		char path[MAX_PATH];
		GetModuleFileNameA(nullptr, path, MAX_PATH);
		std::string dir = path;
		dir = dir.substr(0, dir.find_last_of('\\'));

		TraceClient client;
		if (!client.Start(device.get(), dir + "\\PathTracerHelper.exe", dir, false))
		{
			printf("FAIL %s\n", client.Error().c_str());
			return 1;
		}
		printf("helper: %s, ray tracing %s, textures %s\n", client.Status().DeviceName,
			client.Status().RayTracing ? "yes" : "no", client.Status().CanSampleTextures ? "yes" : "no");

		// The scene: a checkered glossy floor, a red box, a grey wall.
		SceneGeometry world;
		AddQuad(world, vec3(-600, -600, 0), vec3(600, -600, 0), vec3(600, 600, 0), vec3(-600, 600, 0), vec3(0.8f, 0.8f, 0.8f), 0, 8);
		AddBox(world, vec3(-80, -80, 0), vec3(80, 80, 160), vec3(0.8f, 0.15f, 0.1f));
		AddQuad(world, vec3(-600, 400, 0), vec3(600, 400, 0), vec3(600, 400, 600), vec3(-600, 400, 600), vec3(0.6f, 0.6f, 0.6f), -1, 1);
		std::vector<uint32_t> checker(64 * 64);
		for (int y = 0; y < 64; y++)
			for (int x = 0; x < 64; x++)
				checker[y * 64 + x] = (((x / 8) ^ (y / 8)) & 1) ? 0xffe0e0e0u : 0xff303030u;

		SceneInstance placed = {};
		placed.GeometryIndex = 0;
		placed.Transform[0] = placed.Transform[5] = placed.Transform[10] = 1.0f;
		std::vector<SceneInstance> instances = { placed };

		SceneLight light = {};
		light.PositionRadius = vec4(-200, -250, 350, 1400);
		light.ColorBrightness = vec4(1.0f, 0.9f, 0.8f, 2.5f);
		light.DirectionCone = vec4(0, 0, 0, -1);
		light.Flags = vec4(0, 0, 0, -1);
		std::vector<SceneLight> lights = { light };

		client.ResetScene();
		client.Texture(0, 64, 64, checker.data(), vec4(0.3f, 0.0f, 0.04f, 0.0f), false);
		client.Geometry(0, world);

		// The camera, as the render device builds it: right and "up" - which
		// points down the screen - scaled to the view's half extents.
		const vec3 eye(0, -700, 260), target(0, 0, 80);
		const vec3 forward = Normalized(vec3(target.x - eye.x, target.y - eye.y, target.z - eye.z));
		const vec3 right = Normalized(Cross(forward, vec3(0, 0, 1)));
		const vec3 down = Cross(forward, right);
		const float halfWidth = 1.0f, aspect = (float)height / (float)width;

		TraceProtocol::TraceCommand frame = {};
		frame.Width = width;
		frame.Height = height;
		frame.MaxSamples = 256;
		frame.Bounces = 3;
		frame.GlossBounces = 1;
		frame.Denoise = 1;
		frame.Materials = 1;
		frame.Timing = 1;
		frame.Exposure = 0.2f + 128 * (2.0f / 255.0f);
		frame.SkyIntensity = 128 * (2.0f / 255.0f);
		frame.Camera[0] = vec4(eye.x, eye.y, eye.z, 1.0f);     // w: the flash's scale, neutral
		frame.Camera[1] = vec4(right.x * halfWidth, right.y * halfWidth, right.z * halfWidth, 0.0f);
		frame.Camera[2] = vec4(down.x * halfWidth * aspect, down.y * halfWidth * aspect, down.z * halfWidth * aspect, 0.0f);
		frame.Camera[3] = vec4(forward.x, forward.y, forward.z, 0.0f);
		for (int i = 0; i < 4; i++)
			frame.PreviousCamera[i] = frame.Camera[i];

		auto pool = CommandPoolBuilder().QueueFamily(device->GraphicsFamily).Create(device.get());
		auto fence = FenceBuilder().Create(device.get());
		const size_t bytes = (size_t)(width * 3 / 2) * (height * 3 / 2) * 8;
		auto readback = BufferBuilder().Size(bytes).Usage(VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU).Create(device.get());

		int arrived = 0;
		double sendMs = 0.0;
		for (int i = 0; i < frames; i++)
		{
			client.Instances(instances, 1);
			client.Lights(lights, {});
			frame.Frame = (uint32_t)i;
			if (i == frames / 2)
			{
				frame.Width = width * 3 / 2;
				frame.Height = height * 3 / 2;
			}
			frame.AccumulatedFrames = (uint32_t)i;
			frame.RestartDenoiser = i == 0;
			LARGE_INTEGER a, b, f;
			QueryPerformanceCounter(&a);
			const bool traced = client.Trace(frame);
			QueryPerformanceCounter(&b);
			QueryPerformanceFrequency(&f);
			sendMs += (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
			if (!traced)
			{
				printf("frame %d: not traced%s%s\n", i, client.Alive() ? "" : " - ", client.Alive() ? "" : client.Error().c_str());
				if (!client.Alive())
					return 1;
				continue;
			}

			// Taken over from the helper exactly as the render device takes it.
			auto commands = pool->createBuffer();
			commands->begin();
			VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
			barrier.image = client.Output();
			barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
			barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
			barrier.dstQueueFamilyIndex = (uint32_t)device->GraphicsFamily;
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			vkCmdPipelineBarrier(commands->buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
			VkBufferImageCopy region = {};
			region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			region.imageExtent = { client.OutputWidth(), client.OutputHeight(), 1 };
			vkCmdCopyImageToBuffer(commands->buffer, client.Output(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->buffer, 1, &region);
			barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
			barrier.srcQueueFamilyIndex = (uint32_t)device->GraphicsFamily;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
			barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			barrier.dstAccessMask = 0;
			vkCmdPipelineBarrier(commands->buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
			commands->end();

			VkSemaphore ready = client.Ready(), released = client.Released();
			VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
			submit.waitSemaphoreCount = 1;
			submit.pWaitSemaphores = &ready;
			submit.pWaitDstStageMask = &stage;
			submit.commandBufferCount = 1;
			submit.pCommandBuffers = &commands->buffer;
			submit.signalSemaphoreCount = 1;
			submit.pSignalSemaphores = &released;
			vkQueueSubmit(device->GraphicsQueue, 1, &submit, fence->fence);
			vkWaitForFences(device->device, 1, &fence->fence, VK_TRUE, UINT64_MAX);
			vkResetFences(device->device, 1, &fence->fence);
			arrived++;

			const auto& s = client.Status();
			if (i == 0 || i == frames / 2 || i == frames - 1)
				printf("frame %d: %ux%u, %u lights, %u textures, %u shapes, %u instances, denoiser %s (%s), GPU build %.2f trace %.2f denoise %.2f composite %.2f ms%s\n",
					i, client.OutputWidth(), client.OutputHeight(), s.LightCount, s.TextureCount, s.BottomCount, s.InstanceCount,
					s.DenoiserActive ? "on" : "off", s.DenoiserStatus, s.GpuBuildMs, s.GpuTraceMs, s.GpuDenoiseMs, s.GpuCompositeMs,
					s.GpuTimed ? "" : " (not timed)");
		}
		printf("%d of %d frames arrived, %.2f ms a frame to send and have traced\n", arrived, frames, sendMs / frames);

		// The last frame, tonemapped already by the helper, as a PPM.
		const uint32_t outWidth = client.OutputWidth(), outHeight = client.OutputHeight();
		const uint16_t* half = (const uint16_t*)readback->Map(0, bytes);
		FILE* out = fopen("helper-test.ppm", "wb");
		fprintf(out, "P6\n%u %u\n255\n", outWidth, outHeight);
		double sum = 0.0;
		for (uint32_t i = 0; i < outWidth * outHeight; i++)
		{
			for (int c = 0; c < 3; c++)
			{
				float v = HalfToFloat(half[i * 4 + c]);
				v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
				sum += v;
				fputc((int)(v * 255.0f + 0.5f), out);
			}
		}
		fclose(out);
		readback->Unmap();
		const double mean = sum / (outWidth * outHeight * 3.0);
		printf("mean brightness %.3f, written to helper-test.ppm\n", mean);

		vkDeviceWaitIdle(device->device);
		const bool ok = arrived == frames && mean > 0.02;
		printf("=> %s\n", ok ? "PASS" : "FAIL");
		return ok ? 0 : 1;
	}
	catch (const std::exception& e)
	{
		printf("FAIL %s\n", e.what());
		return 1;
	}
}
