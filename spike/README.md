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

## vkxshare

Can a 32-bit process show what a 64-bit process ray traced, frame after frame,
without copying it through the CPU?

The table above says where ray tracing is offered: to 64-bit clients everywhere,
to 32-bit ones only under upstream wine. A path traced render device could
keep reading the level in the game's 32-bit process and hand the tracing to a
64-bit helper - if the two can share an image on the GPU and tell each other
when it is ready. That takes `VK_KHR_external_memory_win32` and
`VK_KHR_external_semaphore_win32` on both sides, on the same GPU.

One source, built twice. `vkxshare.exe` (32-bit) is the game's side: it reports
what its own driver offers, starts `vkxshare64.exe` beside it as the helper on
the GPU with the same UUID, and imports the image and two semaphores the helper
exports. Then for 60 frames the helper writes a different pattern into the
image and signals "ready"; the game's side waits for that on the GPU, reads the
image back and checks every texel, and signals "released" for the next frame.
The image is RGBA8 with storage, sampled and transfer usage, as a render target
shared this way would be.

```sh
cmake --build build-deusex --target vkxshare          # 32-bit, the usual build
cmake -S . -B build-x64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=cmake/xwin-clang-cl-x64.cmake
cmake --build build-x64 --target vkxshare             # 64-bit helper
# both .exe files in one folder, then:
wine vkxshare.exe
```

Measured on an RTX 4090, driver 615.71.09, each Proton in a fresh prefix:

| Environment         | 32-bit ray tracing | 64-bit ray tracing | shared, 60 frames | GPU side per handoff |
| ------------------- | ------------------ | ------------------ | ----------------- | -------------------- |
| wine 11.18          | yes                | yes                | intact            | 0.117 ms             |
| Proton Experimental | no                 | yes                | intact            | 0.121 ms             |
| GE-Proton 11-6      | no                 | yes                | intact            | 0.116 ms             |
| Proton-CachyOS      | no                 | yes                | intact            | 0.115 ms             |
| DW-Proton           | no                 | yes                | intact            | 0.117 ms             |
| Windows             | no                 | yes                | intact            | 0.207 ms             |

So under every Proton a 64-bit helper gets the ray tracing the 32-bit game
does not, and the two share an image with nothing lost, on the same GPU (UUID
and LUID agree). That makes a helper process the way to run the path tracer
from Steam, not only under upstream wine. The per handoff figure is the whole
of the game's submission - waiting on the helper's semaphore, taking ownership,
copying 256 KB back and returning it - from submit to fence.

Native Windows does it too, which the table above had left open: NVIDIA's
32-bit ICD imports the 64-bit one's memory exactly as winevulkan does. Both
sides report the same UUID and LUID, an RGBA8 and an RGBA16F storage image and
a semaphore all export and import, and 60 of 60 frames arrive intact. The
handoff costs about 0.21 ms against wine's 0.117, so roughly 1.8 times as much
for the same work - still a small share of a frame, and measured on the same
RTX 4090 on driver 32.0.16.1692.

**The per handoff figure does not scale to a real render target, and the reason
is this spike rather than the design.** Its timed region copies the whole image
into host visible memory so every texel can be checked, which is verification
and not something a render device would ever do - it would sample the shared
image on the GPU or blit it to the swap chain, and never pull it across PCIe.
Raising `kSize` on Windows shows what that copy costs rather than what a
handoff costs:

| Shared image | Bytes  | Per handoff |
| ------------ | ------ | ----------- |
| 256x256      | 256 KB | 0.21 ms     |
| 1024x1024    | 4 MB   | 0.43-0.67 ms|
| 2048x2048    | 16 MB  | 5.7 ms      |

What is representative is the sync floor at the small sizes, where the copy is
nothing: about 0.2 ms.

### At a real render target's size

`vkxshare.exe --present <w> <h>` answers what the copy hid. The helper fills an
RGBA16F image of that size on the GPU each frame, as a trace would, and the
game's side scales it into an 8-bit image of its own as presenting it would,
reading back only a 4x4 block to check the right frame arrived. It then times
the same scale from an unshared image of its own, which a device pays anyway,
so the difference is what the sharing costs:

| Environment         | Size      | Shared    | Own image | Sharing adds |
| ------------------- | --------- | --------- | --------- | ------------ |
| wine 11.18          | 1920x1440 | 0.121 ms  | 0.034 ms  | 0.088 ms     |
| wine 11.18          | 3440x1440 | 0.136 ms  | 0.039 ms  | 0.097 ms     |
| Proton Experimental | 1920x1440 | 0.128 ms  | 0.042 ms  | 0.086 ms     |
| Proton Experimental | 3440x1440 | 0.137 ms  | 0.038 ms  | 0.099 ms     |
| Windows             |           | not yet measured |    |              |

About a tenth of a millisecond, whatever the size: the cost is the semaphore
round trip and the ownership transfer, not the texels, which never leave the
GPU. A frame in the Hong Kong market is 10 to 12 ms, so under one percent of it.
