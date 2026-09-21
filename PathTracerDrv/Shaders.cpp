#include "Precomp.h"
#include "Shaders.h"

std::string Shaders::Trace()
{
	return R"(
		#version 460
		#extension GL_EXT_ray_query : enable

		layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

		layout(binding = 0) uniform accelerationStructureEXT topLevel;
		layout(binding = 1, rgba32f) uniform image2D accumImage;
		layout(binding = 2, rgba16f) uniform image2D outImage;

		struct TriangleAttributes
		{
			vec4 Normal;
			vec4 Albedo;
			vec4 Emission;
			vec4 Ambient;
		};

		struct SceneLight
		{
			vec4 PositionRadius;
			vec4 ColorBrightness;
		};

		layout(binding = 3, std430) readonly buffer Attributes { TriangleAttributes tris[]; };
		layout(binding = 4, std430) readonly buffer Lights { SceneLight lights[]; };
		// Per instance, indexed by the intersection's instance id: what varies
		// by where a shape is rather than by what it is.
		layout(binding = 5, std430) readonly buffer InstanceData { vec4 instanceAmbient[]; };

		layout(push_constant) uniform PushConstants
		{
			vec4 CameraOrigin;    // xyz world position of the eye
			vec4 CameraRight;     // xyz, already scaled by the horizontal half extent
			vec4 CameraUp;        // xyz, already scaled by the vertical half extent
			vec4 CameraForward;   // xyz unit vector down the middle of the view
			uvec4 Counts;         // x frame, y light count, z bounces, w accumulated frames
			vec4 Params;          // x exposure, y sky intensity, z ray epsilon, w unused
		};

		// A small hash based generator. Path tracing needs a different stream per
		// pixel, per frame and per bounce, and wants it cheap rather than good:
		// the error a weak generator leaves shows up as correlation between
		// neighbouring pixels, which the accumulation then averages away.
		uint pcgHash(uint v)
		{
			uint state = v * 747796405u + 2891336453u;
			uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
			return (word >> 22u) ^ word;
		}

		uint rngState;
		float randomFloat()
		{
			rngState = pcgHash(rngState);
			return float(rngState) * (1.0 / 4294967296.0);
		}

		// Build an orthonormal basis around a normal without a branch, so a
		// cosine weighted direction can be rotated into world space.
		void basisFrom(vec3 n, out vec3 t, out vec3 b)
		{
			float s = n.z >= 0.0 ? 1.0 : -1.0;
			float a = -1.0 / (s + n.z);
			float c = n.x * n.y * a;
			t = vec3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
			b = vec3(c, s + n.y * n.y * a, -n.y);
		}

		vec3 cosineDirection(vec3 n)
		{
			float r1 = randomFloat();
			float r2 = randomFloat();
			float phi = 6.2831853 * r1;
			float sinTheta = sqrt(r2);
			vec3 t, b;
			basisFrom(n, t, b);
			return normalize(t * (cos(phi) * sinTheta) + b * (sin(phi) * sinTheta) + n * sqrt(max(0.0, 1.0 - r2)));
		}

		// Is anything between two points? Terminate on the first hit rather than
		// looking for the closest one: a shadow ray only asks whether, not what.
		bool occluded(vec3 origin, vec3 dir, float dist)
		{
			rayQueryEXT rq;
			rayQueryInitializeEXT(rq, topLevel,
				gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT,
				0xFF, origin, Params.z, dir, dist);
			while (rayQueryProceedEXT(rq)) { }
			return rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT;
		}

		// The engine's falloff, which is what its own lightmaps were baked with:
		// linear to zero at the radius rather than an inverse square that never
		// quite reaches it. Keeping it means a level lights the way its author
		// saw it, which matters more here than being physically right.
		// Pick one light in proportion to what it would contribute if nothing
		// were in the way, then trace a single shadow ray at it.
		//
		// Choosing uniformly instead is what made this scene look black. A Deus
		// Ex level holds hundreds of lights and a given surface is in range of
		// perhaps two, so a uniform pick misses almost every time and the few
		// that land are scaled back up by the light count. The average is right
		// and every individual pixel is wrong, which is exactly the salt and
		// pepper of bright speckles on near black.
		//
		// The loop is over every light, but only to weigh them - a distance and
		// a dot product each, no rays. The one shadow ray is still the
		// expensive part, and now it is nearly always aimed at a light that
		// actually reaches the surface.
		vec3 directLight(vec3 position, vec3 normal, vec3 albedo)
		{
			uint count = Counts.y;
			if (count == 0u)
				return vec3(0.0);

			float weightSum = 0.0;
			int chosen = -1;
			float chosenWeight = 0.0;
			vec3 chosenDir = vec3(0.0);
			float chosenDistance = 0.0;
			vec3 chosenValue = vec3(0.0);

			for (uint i = 0u; i < count; i++)
			{
				SceneLight light = lights[i];

				vec3 toLight = light.PositionRadius.xyz - position;
				float distance = length(toLight);
				float radius = light.PositionRadius.w;
				if (distance >= radius || distance <= 0.0001)
					continue;

				vec3 dir = toLight / distance;
				float cosTheta = dot(normal, dir);
				if (cosTheta <= 0.0)
					continue;

				// The engine's own falloff, linear to zero at the radius, which
				// is what its lightmaps were baked with. Keeping it means a
				// level lights the way its author saw it.
				float falloff = 1.0 - distance / radius;
				falloff = falloff * falloff;

				vec3 value = light.ColorBrightness.rgb * (light.ColorBrightness.a * falloff * cosTheta);
				float weight = dot(value, vec3(0.2126, 0.7152, 0.0722));
				if (weight <= 0.0)
					continue;

				weightSum += weight;
				// Reservoir sampling: each candidate replaces the held one with
				// probability equal to its share of the weight seen so far, so
				// one pass leaves a sample drawn in proportion to weight.
				if (randomFloat() < weight / weightSum)
				{
					chosen = int(i);
					chosenWeight = weight;
					chosenDir = dir;
					chosenDistance = distance;
					chosenValue = value;
				}
			}

			if (chosen < 0 || chosenWeight <= 0.0)
				return vec3(0.0);

			if (occluded(position, chosenDir, chosenDistance - Params.z * 2.0))
				return vec3(0.0);

			// Divide by the probability it was chosen with, which is its share
			// of the total weight.
			return albedo * chosenValue * (weightSum / chosenWeight);
		}

		vec3 skyLight(vec3 dir)
		{
			// Standing in for the level's own sky, which is drawn through a
			// portal this scene does not contain. A weak gradient keeps unlit
			// corners from being pure black, which reads as a hole rather than
			// as shadow.
			float t = clamp(dir.z * 0.5 + 0.5, 0.0, 1.0);
			return mix(vec3(0.02, 0.02, 0.03), vec3(0.10, 0.12, 0.16), t) * Params.y;
		}

		void main()
		{
			ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
			ivec2 size = imageSize(outImage);
			if (pixel.x >= size.x || pixel.y >= size.y)
				return;

			rngState = pcgHash(uint(pixel.x) + uint(pixel.y) * 9781u + Counts.x * 26699u);

			// Jitter inside the pixel: this is the whole of the antialiasing,
			// and it costs nothing because the samples are being averaged anyway.
			vec2 jitter = vec2(randomFloat(), randomFloat());
			vec2 uv = (vec2(pixel) + jitter) / vec2(size) * 2.0 - 1.0;

			vec3 origin = CameraOrigin.xyz;
			vec3 direction = normalize(CameraForward.xyz + CameraRight.xyz * uv.x + CameraUp.xyz * uv.y);

			vec3 radiance = vec3(0.0);
			vec3 throughput = vec3(1.0);

			uint bounces = max(Counts.z, 1u);
			for (uint bounce = 0u; bounce < bounces; bounce++)
			{
				rayQueryEXT rq;
				rayQueryInitializeEXT(rq, topLevel, gl_RayFlagsOpaqueEXT, 0xFF, origin, Params.z, direction, 100000.0);
				while (rayQueryProceedEXT(rq)) { }

				if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT)
				{
					radiance += throughput * skyLight(direction);
					break;
				}

				float t = rayQueryGetIntersectionTEXT(rq, true);
				int primitive = rayQueryGetIntersectionPrimitiveIndexEXT(rq, true);

				// Each instance carries the offset of its geometry's shading
				// data as its custom index, so one buffer serves every shape.
				int attributeBase = rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true);
				TriangleAttributes attr = tris[attributeBase + primitive];

				vec3 position = origin + direction * t;

				// Normals are stored in object space, because the same mesh is
				// instanced at whatever orientation the actor happens to have.
				// Rotating by the instance transform is what puts it back in the
				// world - without it every mover and character would be lit as
				// though it had never turned.
				mat4x3 objectToWorld = rayQueryGetIntersectionObjectToWorldEXT(rq, true);
				vec3 normal = normalize(mat3(objectToWorld) * attr.Normal.xyz);

				// Debug: light every instance that is not the static world, so
				// that "the actors are not being drawn" can be told apart from
				// "the actors are drawn and too dark to see". The static world
				// is the only geometry whose attributes start at zero.
				// Albedo only: no lights, no ambient, no bounces. If something is
				// invisible in the finished image but plain here, it is lit
				// wrongly rather than missing.
				if (Params.w > 1.5)
				{
					radiance = attr.Albedo.rgb;
					break;
				}

				if (Params.w > 0.5)
				{
					// Two different questions, told apart by colour.
					//   magenta: an instanced shape was hit and carried its
					//            attribute offset, so everything works.
					//   green:   an instanced shape was hit but its custom index
					//            arrived as zero, so the shading data is being
					//            read from the wrong place.
					//   dim:     the static world, which is instance zero.
					int instanceId = rayQueryGetIntersectionInstanceIdEXT(rq, true);
					if (instanceId > 0)
					{
						// Green for a character, magenta for anything else, so
						// "people are not traced" can be told from "people are
						// traced and shaded black".
						bool isCharacter = instanceAmbient[instanceId].w > 0.5;
						radiance = (isCharacter ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 1.0)) * 4.0;
						break;
					}
					radiance += throughput * attr.Albedo.rgb * 0.1;
					break;
				}

				// These surfaces are single sided in the engine but solid from
				// either direction here, so face the normal back at the ray.
				if (dot(normal, direction) > 0.0)
					normal = -normal;

				radiance += throughput * attr.Emission.rgb;
				radiance += throughput * directLight(position, normal, attr.Albedo.rgb);

				// The zone's ambient. Level surfaces carry their own, because a
				// zone is a property of the surface; an instanced shape takes it
				// from wherever the actor happens to be standing. Without this
				// anything the light actors do not reach is pure black, which is
				// not what the engine shows.
				vec3 ambient = attr.Ambient.rgb + instanceAmbient[rayQueryGetIntersectionInstanceIdEXT(rq, true)].rgb;
				radiance += throughput * attr.Albedo.rgb * ambient;

				throughput *= attr.Albedo.rgb;

				// Russian roulette on the dim paths. Without it the loop spends
				// most of its time on bounces that cannot change the pixel.
				if (bounce >= 2u)
				{
					float p = clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.05, 1.0);
					if (randomFloat() > p)
						break;
					throughput /= p;
				}

				origin = position + normal * Params.z;
				direction = cosineDirection(normal);
			}

			// Average with what has already been traced from this viewpoint.
			// Reset by the device whenever the camera or the scene moves, so a
			// still image converges and a moving one stays responsive.
			uint accumulated = Counts.w;
			vec3 result = radiance;
			if (accumulated > 0u)
			{
				vec3 previous = imageLoad(accumImage, pixel).rgb;
				result = mix(previous, radiance, 1.0 / float(accumulated + 1u));
			}
			imageStore(accumImage, pixel, vec4(result, 1.0));

			// Tonemap and encode here rather than in a present pass: the result
			// is blitted straight to the swap chain, so this is the last thing
			// that happens to the pixel.
			vec3 mapped = result * Params.x;
			mapped = mapped / (mapped + vec3(1.0));
			mapped = pow(max(mapped, vec3(0.0)), vec3(1.0 / 2.2));

			imageStore(outImage, pixel, vec4(mapped, 1.0));
		}
	)";
}

std::string Shaders::TileVertex()
{
	return R"(
		#version 460

		layout(location = 0) in vec2 aPosition;   // already in normalised device coordinates
		layout(location = 1) in vec2 aTexCoord;
		layout(location = 2) in vec4 aColor;

		layout(location = 0) out vec2 vTexCoord;
		layout(location = 1) out vec4 vColor;

		void main()
		{
			vTexCoord = aTexCoord;
			vColor = aColor;
			gl_Position = vec4(aPosition, 0.0, 1.0);
		}
	)";
}

std::string Shaders::TileFragment()
{
	return R"(
		#version 460

		layout(binding = 0) uniform sampler2D texSampler;

		layout(location = 0) in vec2 vTexCoord;
		layout(location = 1) in vec4 vColor;
		layout(location = 0) out vec4 outColor;

		void main()
		{
			vec4 texel = texture(texSampler, vTexCoord);

			// The traced image is tonemapped and gamma encoded by the time the
			// tiles land on it, so this pass works in display space too and the
			// engine's colours can be used as they are given.
			outColor = texel * vColor;

			// Masked art arrives with a zero alpha hole; anything that survives
			// a blend at all should not be written where the hole is.
			if (outColor.a < 0.01)
				discard;
		}
	)";
}
