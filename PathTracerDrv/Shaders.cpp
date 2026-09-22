#include "Precomp.h"
#include "Shaders.h"

std::string Shaders::Trace()
{
	return R"(
		#version 460
		#extension GL_EXT_ray_query : enable
		// Each ray lands on whatever triangle it lands on, so the texture index
		// differs between neighbouring invocations.
		#extension GL_EXT_nonuniform_qualifier : enable

		layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

		layout(binding = 0) uniform accelerationStructureEXT topLevel;
		layout(binding = 1, rgba32f) uniform image2D accumImage;
		layout(binding = 2, rgba16f) uniform image2D outImage;
		// xyz: the world position this pixel hit last frame. w: which instance
		// owned it, or -1 for the sky.
		layout(binding = 7, rgba32f) uniform image2D historyImage;

		struct TriangleAttributes
		{
			vec4 Normal;
			vec4 Albedo;
			vec4 Emission;
			vec4 Ambient;
			vec4 UV01;      // u0 v0 u1 v1
			vec4 UV2Tex;    // u2 v2 texture unused
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
		// Sized to match the layout rather than left open: an unsized array needs
		// the runtime descriptor array capability, and a device without it would
		// fail to create the pipeline at all rather than simply not texture.
		layout(binding = 6) uniform sampler2D sceneTextures[1024];

		layout(push_constant) uniform PushConstants
		{
			vec4 CameraOrigin;    // xyz world position of the eye
			vec4 CameraRight;     // xyz, already scaled by the horizontal half extent
			vec4 CameraUp;        // xyz, already scaled by the vertical half extent
			vec4 CameraForward;   // xyz unit vector down the middle of the view
			uvec4 Counts;         // x frame, y light count, z bounces, w accumulated frames
			vec4 Params;          // x exposure, y sky intensity, z ray epsilon, w debug mode
			uint TextureCount;    // 0 when the device cannot index the array
			uint MaxSamples;      // ceiling on samples averaged into one pixel
		};

		// Does this point on the triangle actually exist? UE1 masked art keys
		// transparency to palette index zero, which the upload turns into an
		// alpha of zero. Rendering those texels rather than seeing through them
		// is what put magenta - the key colour in Deus Ex's packages - across
		// every window and railing.
		// The triangle's own colour, textured where there is a texture to read.
		// UE1 surface coordinates run well outside 0..1 - one texture tiles
		// across a whole wall - which the sampler's repeat mode handles.
		vec3 surfaceAlbedo(TriangleAttributes attr, vec2 bary)
		{
			int index = int(attr.UV2Tex.z);
			if (index < 0 || uint(index) >= TextureCount)
				return attr.Albedo.rgb;

			// Barycentrics from a ray query are the weights of the second and
			// third vertices; the first takes up the remainder.
			vec2 uv = attr.UV01.xy * (1.0 - bary.x - bary.y)
			        + attr.UV01.zw * bary.x
			        + attr.UV2Tex.xy * bary.y;

			vec4 texel = texture(sceneTextures[nonuniformEXT(index)], uv);

			// The engine's art is authored in sRGB; the trace works in linear.
			vec3 linearRgb = pow(max(texel.rgb, vec3(0.0)), vec3(2.2));
			return linearRgb;
		}

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

		// Should traversal accept this candidate triangle?
		//
		//   opaque      always.
		//   masked      only where the texture's alpha says the texel is there.
		//               UE1 keys transparency to palette index zero, and drawing
		//               those texels put the key colour across every grate.
		//   translucent always for a view ray, which then adds the surface's
		//               colour and carries on through it; never for a shadow
		//               ray, since glass does not stop light.
		//
		// Confirming rather than tinting during traversal. A ray query reports
		// candidates in whatever order traversal reaches them, including ones
		// beyond what turns out to be the closest opaque hit, so accumulating
		// anything as the ray passes let glass behind a wall colour the pixel in
		// front of it. Confirming shrinks the ray properly and keeps the hits in
		// order.
		bool confirmCandidate(int attributeBase, int primitive, vec2 bary, bool shadowRay)
		{
			TriangleAttributes attr = tris[attributeBase + primitive];

			// Sprites cast no shadows: the engine draws them flat onto the
			// screen, after the world, and a quad turned to face the camera
			// would throw a shadow that swings as the view turns.
			if (shadowRay && attr.Emission.w > 1.5)
				return false;

			float kind = attr.UV2Tex.w;
			if (kind < 0.5)
				return true;

			int index = int(attr.UV2Tex.z);
			bool textured = index >= 0 && uint(index) < TextureCount;

			vec2 uv = attr.UV01.xy * (1.0 - bary.x - bary.y)
			        + attr.UV01.zw * bary.x
			        + attr.UV2Tex.xy * bary.y;

			if (kind < 1.5)
				return textured ? texture(sceneTextures[nonuniformEXT(index)], uv).a > 0.5 : true;

			// A mirror is solid; it reflects rather than letting anything past.
			if (kind > 2.5)
				return true;

			// Translucent adds and modulated multiplies; both are handled at the
			// hit and neither stops light.
			return !shadowRay;
		}

		// Is anything between two points? Terminate on the first hit rather than
		// looking for the closest one: a shadow ray only asks whether, not what.
		bool occluded(vec3 origin, vec3 dir, float dist)
		{
			rayQueryEXT rq;
			rayQueryInitializeEXT(rq, topLevel,
				gl_RayFlagsTerminateOnFirstHitEXT,
				0xFF, origin, Params.z, dir, dist);
			// A hole in a grate lets light through, so a candidate only counts
			// as occluding once its texel is known to be there.
			// Light passes through glass and through the holes in a grate, so a
			// candidate only occludes once it is confirmed.
			while (rayQueryProceedEXT(rq))
			{
				if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT)
				{
					if (confirmCandidate(
							rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false),
							rayQueryGetIntersectionPrimitiveIndexEXT(rq, false),
							rayQueryGetIntersectionBarycentricsEXT(rq, false),
							true))
						rayQueryConfirmIntersectionEXT(rq);
				}
			}
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

				// Linear to zero at the radius. The comment here used to say
				// linear and then square it, which is the curve the baked
				// lightmaps used rather than the one the engine applies to
				// dynamic lighting. Squaring it costs most of a light's useful
				// range: the player's light augmentation has a radius of only
				// 100 units, so at one metre it had already fallen to a quarter
				// and at two metres to nothing.
				float falloff = 1.0 - distance / radius;

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
			// Passing through a translucent surface is not a bounce: a window
			// with a pane and a frame would otherwise use up the ray's budget
			// before it reached anything solid.
			uint passes = 0u;
			// How far along the ray to start looking. Generous for a bounce off
			// a surface, because these levels are big and a surface acne
			// artefact is worse than a lost millimetre - but tiny when carrying
			// on through a surface, since the thing behind it may be flush
			// against it. A laser dot sits on a wall, and stepping a whole unit
			// past it skipped the wall entirely and put a hole in the level.
			float rayMin = Params.z;

			// What this pixel is looking at, recorded on the first bounce so the
			// accumulated history can be checked against it.
			vec3 primaryPosition = origin + direction * 100000.0;
			float primaryInstance = -1.0;
			float primaryDistance = 100000.0;

			uint bounces = max(Counts.z, 1u);
			for (uint bounce = 0u; bounce < bounces; bounce++)
			{
				rayQueryEXT rq;
				rayQueryInitializeEXT(rq, topLevel, gl_RayFlagsNoneEXT, 0xFF, origin, rayMin, direction, 100000.0);
				while (rayQueryProceedEXT(rq))
				{
					// Only geometry holding masked or translucent art is
					// non-opaque, so this runs for grates, windows and glass and
					// nothing else.
					if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT)
					{
						if (confirmCandidate(
								rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false),
								rayQueryGetIntersectionPrimitiveIndexEXT(rq, false),
								rayQueryGetIntersectionBarycentricsEXT(rq, false),
								false))
							rayQueryConfirmIntersectionEXT(rq);
					}
				}

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

				if (bounce == 0u)
				{
					primaryDistance = t;
					primaryPosition = origin + direction * t;
					primaryInstance = float(rayQueryGetIntersectionInstanceIdEXT(rq, true));
					// A surface whose texture animates has nothing worth reusing
					// from earlier frames: averaging a screen against what it
					// showed a second ago is what made them look frozen while
					// standing still.
					if (attr.Albedo.w > 0.5)
						primaryInstance = -2.0 - float(Counts.x);
				}
				vec2 bary = rayQueryGetIntersectionBarycentricsEXT(rq, true);
				attr.Albedo = vec4(surfaceAlbedo(attr, bary), attr.Albedo.w);

				vec3 position = origin + direction * t;

				// Normals are stored in object space, because the same mesh is
				// instanced at whatever orientation the actor happens to have.
				// Rotating by the instance transform is what puts it back in the
				// world - without it every mover and character would be lit as
				// though it had never turned.
				mat4x3 objectToWorld = rayQueryGetIntersectionObjectToWorldEXT(rq, true);
				vec3 normal = normalize(mat3(objectToWorld) * attr.Normal.xyz);

				// Translucent: add what this surface contributes and carry on
				// through it in the same direction. UE1 draws these additively,
				// which is why the muzzle flash quad on a weapon is invisible
				// until it is lit and why a red dot sight glows rather than
				// showing as a dark blob.
				float kind = attr.UV2Tex.w;

				// A sprite is as bright as its actor's ScaleGlow, which the
				// instance carries in place of an ambient it has no use for.
				bool sprite = attr.Emission.w > 1.5;
				float glow = sprite ? instanceAmbient[rayQueryGetIntersectionInstanceIdEXT(rq, true)].x : 1.0;

				if ((kind > 1.5 && kind < 2.5) || kind > 3.5)
				{
					if (kind > 3.5)
					{
						// Modulated: the surface multiplies what is behind it,
						// as modulate-2x, so mid grey leaves the background
						// alone. Treating it as additive along with translucent
						// turned a pair of dark sunglasses bright white.
						throughput *= clamp(attr.Albedo.rgb * 2.0, vec3(0.0), vec3(1.0));
					}
					else
					{
						// Translucent: additive, so black is invisible and
						// bright glows.
						radiance += throughput * attr.Albedo.rgb * glow;
					}

					origin = position;
					rayMin = 0.01;
					if (passes < 8u)
					{
						passes++;
						bounce--;
					}
					continue;
				}

				// A solid or masked sprite is just its picture: nothing lights it
				// and nothing bounces off it.
				if (sprite && Params.w < 1.5)
				{
					radiance += throughput * attr.Albedo.rgb * glow;
					break;
				}

				// A reflective surface - the polished floor of the UNATCO lobby
				// is the one that shows. Half the rays carry on in the mirrored
				// direction and half shade the surface itself, which averages
				// out to a floor that is both marble and a reflection. Without
				// this the flag meant nothing and the floor was a flat slab of
				// whatever colour its texture averaged to.
				if (attr.UV2Tex.w > 2.5 && randomFloat() < 0.5)
				{
					// Tinted by the floor but not dimmed to nothing by it: dark
					// marble has an albedo near 0.1, and multiplying the
					// reflection by that made it invisible.
					throughput *= 0.55 + 0.45 * clamp(attr.Albedo.rgb * 2.5, vec3(0.0), vec3(1.0));
					origin = position + normal * Params.z;
					rayMin = Params.z;
					direction = reflect(direction, normal);
					continue;
				}

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

				// Self lit surfaces emit the colour they actually are, which is
				// only known once the texture has been sampled. Emission.w is
				// the flag; the rgb carries nothing.
				if (attr.Emission.w > 0.5)
					radiance += throughput * attr.Albedo.rgb;
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
				rayMin = Params.z;
				direction = cosineDirection(normal);
			}

			// Average with what has already been traced from this viewpoint.
			// Reset by the device whenever the camera or the scene moves, so a
			// still image converges and a moving one stays responsive.
			// Accumulation is judged per pixel rather than for the whole frame.
			// The device resets everything when the camera moves, but an actor
			// walking past moves nothing the device can see: the instance count
			// does not change and neither does the view, so those pixels went on
			// averaging against the wall behind them. That is what turned a
			// moving character into a fading smear.
			vec4 stored = imageLoad(accumImage, pixel);
			vec4 previousHit = imageLoad(historyImage, pixel);

			float samples = stored.a;
			if (Counts.w == 0u)
			{
				// The device invalidated everything, typically a camera move.
				samples = 0.0;
			}
			else if (previousHit.w != primaryInstance)
			{
				// A different object is under this pixel than last frame.
				samples = 0.0;
			}
			else if (primaryInstance >= 0.0 && instanceAmbient[int(primaryInstance)].w > 0.5)
			{
				// The scene says this instance moved or changed shape since the
				// last frame, so nothing accumulated for it still describes it.
				// Exact, where judging by how far the hit point shifted was not:
				// anything slower than the tolerance kept its history and
				// smeared, which is what a medical bot crossing a room does.
				samples = 0.0;
			}
			else
			{
				// The same object, in the same place. The hit point still wanders
				// by about a pixel's footprint because the ray is jittered inside
				// the pixel, which is all this tolerance is for.
				float tolerance = max(0.05, primaryDistance * 0.004);
				if (distance(previousHit.xyz, primaryPosition) > tolerance)
					samples = 0.0;
			}

			vec3 result = radiance;
			if (samples > 0.0)
				result = mix(stored.rgb, radiance, 1.0 / (samples + 1.0));

			samples = min(samples + 1.0, float(max(MaxSamples, 1u)));
			imageStore(accumImage, pixel, vec4(result, samples));
			imageStore(historyImage, pixel, vec4(primaryPosition, primaryInstance));

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
