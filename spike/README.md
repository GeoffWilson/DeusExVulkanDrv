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

**That measurement was taken in the wrong place.** It used the distribution's
own wine. The game runs under Proton, whose winevulkan does not pass the ray
tracing extensions through to a 32 bit client at all:

| Environment            | ray tracing | device extensions |
| ---------------------- | ----------- | ----------------- |
| wine 11.17             | yes         | 276               |
| Proton-CachyOS         | no          | 250               |
| GE-Proton 10-34 … 11-6 | no          | 249-262           |
| DW-Proton              | no          | 259               |

Every Proton on the machine was tested; none offer them. So the environment has
to be measured, not the GPU - run this under the same wine and prefix the game
will use:

```sh
WINEPREFIX=~/.local/share/Steam/steamapps/compatdata/<appid>/pfx   "<proton>/files/bin/wine" build-deusex/vkrtcheck.exe
```

### Native Windows

Measured, and the guess above it was wrong. There is no translation layer in the
way, so the 32 bit client is asked of the driver itself - and NVIDIA's 32 bit
Windows ICD does not offer the ray tracing extensions at all:

	pointer size: 32 bits
	--- NVIDIA GeForce RTX 4090 (api 1.4.351) ---
	  VK_KHR_acceleration_structure                no
	  VK_KHR_ray_tracing_pipeline                  no
	  VK_KHR_ray_query                             no
	  VK_KHR_deferred_host_operations              no
	  VK_KHR_buffer_device_address                 YES
	  VK_EXT_descriptor_indexing                   YES
	  => hardware ray tracing from a 32-bit client: NOT AVAILABLE
	  (270 device extensions in total)

The same spike built for x64 and run on the same machine, against the same
driver, is offered all four and 289 extensions in total. So it is the ICD's
bitness that decides it, not the GPU, the driver version or the machine:

| Environment                   | ray tracing | device extensions |
| ----------------------------- | ----------- | ----------------- |
| Windows, 32 bit (driver 32.0.16.1692) | no  | 270               |
| Windows, 64 bit (same driver) | yes         | 289               |
| wine 11.17                    | yes         | 276               |
| Proton (every build tested)   | no          | 249-262           |

Which inverts the expectation the rest of this file was written under. Wine is
not an obstacle the path tracer survives - it is the reason it runs at all,
because winevulkan thunks a 32 bit client's calls through to the 64 bit driver
and so hands it a device the native 32 bit ICD never exposes. Proton's does not
thunk them through; Windows has nothing to thunk.

Build and run the x64 comparison with:

```sh
cmake -S . -B build-win64-spike -G Ninja      # from an x64 developer prompt
cmake --build build-win64-spike --target vkrtcheck
build-win64-spike/vkrtcheck.exe
```
