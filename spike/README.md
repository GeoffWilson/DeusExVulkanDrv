# Spikes

Throwaway programs that answer one question about the environment. They are not
part of any driver and are excluded from the default build.

## vkrtcheck

Does a 32-bit Vulkan client get hardware ray tracing on this machine?

Deus Ex is a 32-bit process, so a path traced render device would have to build
acceleration structures from inside one. That the 32-bit ICD works at all is
already known - VulkanDrv runs - but whether the driver advertises the ray
tracing extensions to a 32-bit client is a separate question, and the answer
decides whether such a device could use the hardware or would have to carry its
own BVH in compute.

It loads the loader by hand, so it needs no import library and reports what a
render device would actually see.

```sh
cmake --build build-deusex --target vkrtcheck
wine build-deusex/vkrtcheck.exe          # or run it on Windows directly
```

Measured on an RTX 4090, driver 615.71.09, under wine 11.17 on Linux:

	pointer size: 32 bits
	--- NVIDIA GeForce RTX 4090 (api 1.4.351) ---
	  VK_KHR_acceleration_structure                YES
	  VK_KHR_ray_tracing_pipeline                  YES
	  VK_KHR_ray_query                             YES
	  VK_KHR_deferred_host_operations              YES
	  VK_KHR_buffer_device_address                 YES
	  VK_EXT_descriptor_indexing                   YES
	  max geometry count      16777215
	  max instance count      16777215
	  max primitive count     536870911
	  => hardware ray tracing from a 32-bit client: AVAILABLE

`ray_query` being present is the useful part: it allows tracing from an ordinary
compute or fragment shader, without a ray tracing pipeline and its shader
binding table. The limits are nowhere near a constraint - a whole Deus Ex level
is a few thousand BSP surfaces.

Not yet measured on native Windows. The driver is the same codebase, so the
answer is very likely the same, but it is one command to confirm.
