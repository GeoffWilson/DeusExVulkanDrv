// Does a 32-bit Vulkan client get hardware ray tracing on this machine?
//
// Deus Ex is a 32-bit process, so a path tracing render device would have to
// build acceleration structures from inside one. The 32-bit ICD works - the
// VulkanDrv port proves that - but whether the driver advertises the ray
// tracing extensions to a 32-bit client is a separate question, and the answer
// decides whether a path traced device can use the hardware or has to carry its
// own BVH in compute.
//
// Loads the loader by hand so it needs no import library: it reports what a
// render device would actually see, nothing more.

#include <vulkan/vulkan.h>
#include <windows.h>
#include <cstdio>
#include <vector>
#include <string>

static const char* kWanted[] = {
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
    VK_KHR_RAY_QUERY_EXTENSION_NAME,
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
    VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
    VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
};

int main()
{
    std::printf("pointer size: %d bits\n\n", (int)(sizeof(void*) * 8));

    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    if (!lib) { std::printf("FAIL: vulkan-1.dll would not load\n"); return 1; }

    auto getInstanceProc = (PFN_vkGetInstanceProcAddr)GetProcAddress(lib, "vkGetInstanceProcAddr");
    if (!getInstanceProc) { std::printf("FAIL: no vkGetInstanceProcAddr\n"); return 1; }

    auto createInstance = (PFN_vkCreateInstance)getInstanceProc(nullptr, "vkCreateInstance");

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "vkrtcheck";
    app.apiVersion = VK_API_VERSION_1_1;   // what VulkanDrv asks for

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;

    VkInstance instance = VK_NULL_HANDLE;
    VkResult r = createInstance(&ici, nullptr, &instance);
    if (r != VK_SUCCESS) { std::printf("FAIL: vkCreateInstance returned %d\n", (int)r); return 1; }

    auto enumDevices = (PFN_vkEnumeratePhysicalDevices)getInstanceProc(instance, "vkEnumeratePhysicalDevices");
    auto getProps = (PFN_vkGetPhysicalDeviceProperties)getInstanceProc(instance, "vkGetPhysicalDeviceProperties");
    auto enumExts = (PFN_vkEnumerateDeviceExtensionProperties)getInstanceProc(instance, "vkEnumerateDeviceExtensionProperties");
    auto getProps2 = (PFN_vkGetPhysicalDeviceProperties2)getInstanceProc(instance, "vkGetPhysicalDeviceProperties2");

    uint32_t count = 0;
    enumDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    enumDevices(instance, &count, devices.data());
    std::printf("%u physical device(s)\n\n", count);

    for (VkPhysicalDevice dev : devices)
    {
        VkPhysicalDeviceProperties p{};
        getProps(dev, &p);
        std::printf("--- %s (api %u.%u.%u) ---\n", p.deviceName,
                    VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion));

        uint32_t n = 0;
        enumExts(dev, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> exts(n);
        enumExts(dev, nullptr, &n, exts.data());

        bool haveAccel = false, havePipeline = false;
        for (const char* want : kWanted)
        {
            bool found = false;
            for (const auto& e : exts)
                if (std::string(e.extensionName) == want) { found = true; break; }
            std::printf("  %-44s %s\n", want, found ? "YES" : "no");
            if (found && std::string(want) == VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) haveAccel = true;
            if (found && std::string(want) == VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) havePipeline = true;
        }

        if (haveAccel && getProps2)
        {
            VkPhysicalDeviceAccelerationStructurePropertiesKHR accel{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
            VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            p2.pNext = &accel;
            getProps2(dev, &p2);
            std::printf("  max geometry count      %llu\n", (unsigned long long)accel.maxGeometryCount);
            std::printf("  max instance count      %llu\n", (unsigned long long)accel.maxInstanceCount);
            std::printf("  max primitive count     %llu\n", (unsigned long long)accel.maxPrimitiveCount);
        }

        std::printf("  => hardware ray tracing from a 32-bit client: %s\n\n",
                    (haveAccel && havePipeline) ? "AVAILABLE" : "NOT AVAILABLE");
        std::printf("  (%u device extensions in total)\n\n", n);
    }

    return 0;
}
