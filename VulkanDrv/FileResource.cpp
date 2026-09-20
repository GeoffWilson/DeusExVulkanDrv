
#include "Precomp.h"
#include "FileResource.h"

// I probably should find a less brain dead way of doing this. :)

std::string FileResource::readAllText(const std::string& filename)
{
	if (filename == "shaders/Scene.vert")
	{
		return R"(
			layout(push_constant) uniform ScenePushConstants
			{
				mat4 objectToProjection;
				vec4 nearClip;
				uint uHitIndex;
				uint uSrgbLight;
				uint padding2, padding3;
			};

			layout(location = 0) in uint aFlags;
			layout(location = 1) in vec3 aPosition;
			layout(location = 2) in vec2 aTexCoord;
			layout(location = 3) in vec2 aTexCoord2;
			layout(location = 4) in vec2 aTexCoord3;
			layout(location = 5) in vec2 aTexCoord4;
			layout(location = 6) in vec4 aColor;
			layout(location = 7) in ivec4 aTextureBinds;

			layout(location = 0) flat out uint flags;
			layout(location = 1) out vec2 texCoord;
			layout(location = 2) out vec2 texCoord2;
			layout(location = 3) out vec2 texCoord3;
			layout(location = 4) out vec2 texCoord4;
			layout(location = 5) out vec4 color;
			layout(location = 6) flat out uint hitIndex;
			layout(location = 7) flat out ivec4 textureBinds;

			void main()
			{
				gl_Position = objectToProjection * vec4(aPosition, 1.0);
				gl_ClipDistance[0] = dot(nearClip, vec4(aPosition, 1.0));
				flags = aFlags;
				texCoord = aTexCoord;
				texCoord2 = aTexCoord2;
				texCoord3 = aTexCoord3;
				texCoord4 = aTexCoord4;
				color = aColor;
				hitIndex = uHitIndex;
				textureBinds = aTextureBinds;
			}
		)";
	}
	else if (filename == "shaders/Scene.frag")
	{
		return R"(
			layout(push_constant) uniform ScenePushConstants
			{
				mat4 objectToProjection;
				vec4 nearClip;
				uint uHitIndex;
				uint uSrgbLight;
				uint padding2, padding3;
			};

			layout(binding = 0) uniform sampler2D textures[];

			layout(location = 0) flat in uint flags;
			layout(location = 1) centroid in vec2 texCoord;
			layout(location = 2) in vec2 texCoord2;
			layout(location = 3) in vec2 texCoord3;
			layout(location = 4) in vec2 texCoord4;
			layout(location = 5) in vec4 color;
			layout(location = 6) flat in uint hitIndex;
			layout(location = 7) flat in ivec4 textureBinds;

			layout(location = 0) out vec4 outColor;
			layout(location = 1) out uint outHitIndex;

			vec4 darkClamp(vec4 c)
			{
				// Make all textures a little darker as some of the textures (i.e coronas) never become completely black as they should have
				float cutoff = 3.1/255.0;
				return vec4(clamp((c.rgb - cutoff) / (1.0 - cutoff), 0.0, 1.0), c.a);
			}

			// With sRGB textures the sampler hands back linear light, but the light
			// values that arrive as vertex colours - lighting, fog, tile colours -
			// are still in display space. Multiplying the two mixes spaces and the
			// result is neither; bring them across as well.
			vec3 toSceneSpace(vec3 c)
			{
				return uSrgbLight != 0 ? pow(max(c, 0.0), vec3(2.2)) : c;
			}

			// The engine's brightness multipliers - the doubling that compensates
			// for half brightness light maps, and the actor boost - are figures
			// for display space. Applied to linear values and encoded back, a
			// doubling arrives as 2^(1/2.2), which is 1.37: everything lit comes
			// out a third too dark. Take them across the same way.
			float toSceneSpace(float scale)
			{
				return uSrgbLight != 0 ? pow(scale, 2.2) : scale;
			}

			vec4 textureTex(vec2 uv) { return texture(textures[nonuniformEXT(textureBinds.x)], uv); }
			vec4 textureMacro(vec2 uv) { return texture(textures[nonuniformEXT(textureBinds.y)], uv); }
			vec4 textureDetail(vec2 uv) { return texture(textures[nonuniformEXT(textureBinds.z)], uv); }
			vec4 textureLightmap(vec2 uv) { return texture(textures[nonuniformEXT(textureBinds.w)], uv); }

			void main()
			{
				float actorXBlending = toSceneSpace((flags & 32) != 0 ? 1.5 : 1.0);
				float oneXBlending = toSceneSpace((flags & 64) != 0 ? 1.0 : 2.0);

				outColor = darkClamp(textureTex(texCoord)) * vec4(toSceneSpace(color.rgb), color.a);
				outColor.rgb *= actorXBlending;

				if ((flags & 2) != 0) // Macro texture
				{
					outColor *= darkClamp(textureMacro(texCoord3));
				}

				if ((flags & 1) != 0) // Lightmap
				{
					outColor.rgb *= clamp(textureLightmap(texCoord2).rgb, 0.0, 1.0) * oneXBlending;
				}

				if ((flags & 4) != 0) // Detail texture
				{
					float fadedistance = 380.0f;
					float a = clamp(2.0f - (1.0f / gl_FragCoord.w) / fadedistance, 0.0f, 1.0f);
					vec4 detailColor = (textureDetail(texCoord4) - 0.5) * 0.8 + 1.0;
					detailColor.rgb = uSrgbLight != 0 ? pow(max(detailColor.rgb, 0.0), vec3(2.2)) : detailColor.rgb;
					outColor.rgb = mix(outColor.rgb, outColor.rgb * detailColor.rgb, a);
				}
				else if ((flags & 8) != 0) // Fog map
				{
					vec4 fogcolor = textureDetail(texCoord4);
					outColor.rgb = fogcolor.rgb + outColor.rgb * (1.0 - fogcolor.a);
				}
				else if ((flags & 16) != 0) // Fog color
				{
					vec4 fogcolor = vec4(toSceneSpace(vec3(texCoord2, texCoord3.x)), texCoord3.y);
					outColor.rgb = fogcolor.rgb + outColor.rgb * (1.0 - fogcolor.a);
				}

				#if defined(ALPHATEST)
					#if defined(ALPHATOCOVERAGE)
						// The hardware turns this alpha into a coverage mask, so
						// rescale it to cross the 0.5 threshold over about one
						// pixel. Left as the texture gives it, the cutout fades
						// across however wide the mip filtering smeared the
						// edge, which reads as a blurred fringe rather than an
						// anti aliased one.
						outColor.a = clamp((outColor.a - 0.5) / max(fwidth(outColor.a), 0.0001) + 0.5, 0.0, 1.0);
						if (outColor.a <= 0.0) discard;
					#else
						if (outColor.a < 0.5) discard;
					#endif
				#endif

				outColor = clamp(outColor, 0.0, 1.0);

				outHitIndex = hitIndex;
			}
		)";
	}
	else if (filename == "shaders/PPStep.vert")
	{
		return R"(
			layout(location = 0) out vec2 texCoord;

			vec2 positions[6] = vec2[](
				vec2(-1.0, -1.0),
				vec2( 1.0, -1.0),
				vec2(-1.0,  1.0),
				vec2(-1.0,  1.0),
				vec2( 1.0, -1.0),
				vec2( 1.0,  1.0)
			);

			void main()
			{
				vec4 pos = vec4(positions[gl_VertexIndex], 0.0, 1.0);
				gl_Position = pos;
				texCoord = pos.xy * 0.5 + 0.5;
			}
		)";
	}
	else if (filename == "shaders/Present.frag")
	{
		return R"(
			layout(push_constant) uniform PresentPushConstants
			{
				float Contrast;
				float Saturation;
				float Brightness;
				float HdrScale;
				vec4 GammaCorrection;
			};

			layout(binding = 0) uniform sampler2D texSampler;
			layout(binding = 1) uniform sampler2D texDither;
			layout(location = 0) in vec2 texCoord;
			layout(location = 0) out vec4 outColor;

			vec3 dither(vec3 c)
			{
				vec2 texSize = vec2(textureSize(texDither, 0));
				float threshold = texture(texDither, gl_FragCoord.xy / texSize).r;
				return floor(c.rgb * 255.0 + threshold) / 255.0;
			}

			vec3 linearHdr(vec3 c)
			{
				return pow(c, vec3(2.2)) * HdrScale;
			}

			#if defined(GAMMA_MODE_D3D9)

			vec3 gammaCorrect(vec3 c)
			{
				return pow(c, GammaCorrection.xyz);
			}

			#elif defined(GAMMA_MODE_XOPENGL)

			// Returns maximum of first 3 components
			float max3(vec3 v)
			{
				return max(max(v.x, v.y), v.z);
			}
			float max3(vec4 v)
			{
				return max(max(v.x, v.y), v.z);
			}

			// Returns square of argument
			float square_f( float f)
			{
				return f*f;
			}

			vec3 gammaCorrect(vec3 c)
			{
				c = clamp(c, 0.0, 1.0); // XOpenGLDrv doesn't use a half-float scene buffer

				if (GammaCorrection.w > 1.0)
				{
					// Obtains a multiplier required to offset value according to the following
					// formula: ((1 - (2 * value - 1)^2) * 0.25)
					// It has the shape of a parabola with roots in 0,1 and maximum at f(x=0.5)=0.25
					float CCValue = max(max3(c), 0.001);
					float CC = (1.0 - square_f(2.0 * CCValue - 1.0)) * 0.25  * (GammaCorrection.w - 1.0);
					c = clamp( c * ((CCValue+CC) / CCValue), 0.0, 1.0);
				}
				else if (GammaCorrection.w < 1.0)
				{
					// Downscale brightness
					c *= GammaCorrection.w;
				}

				return pow(c, GammaCorrection.xyz);
			}

			#endif

			#if defined(COLOR_CORRECT_MODE0)
			vec3 colorCorrect(vec3 c)
			{
				float v = c.r + c.g + c.b;
				vec3 valgray = vec3(v, v, v) * (1 - Saturation) / 3 + c * Saturation;
				vec3 val = valgray * Contrast - (Contrast - 1.0) * 0.5;
				val += Brightness * 0.5;
				return max(val, vec3(0.0, 0.0, 0.0));
			}
			#elif defined(COLOR_CORRECT_MODE1)
			vec3 colorCorrect(vec3 c)
			{
				float v = dot(c, vec3(0.3, 0.56, 0.14));
				vec3 valgray = mix(vec3(v, v, v), c, Saturation);
				vec3 val = valgray * Contrast - (Contrast - 1.0) * 0.5;
				val += Brightness * 0.5;
				return max(val, vec3(0.0, 0.0, 0.0));
			}
			#elif defined(COLOR_CORRECT_MODE2)
			vec3 colorCorrect(vec3 c)
			{
				float v = pow(dot(pow(c, vec3(2.2, 2.2, 2.2)), vec3(0.2126, 0.7152, 0.0722)), 1.0/2.2);
				vec3 valgray = mix(vec3(v, v, v), c, Saturation);
				vec3 val = valgray * Contrast - (Contrast - 1.0) * 0.5;
				val += Brightness * 0.5;
				return max(val, vec3(0.0, 0.0, 0.0));
			}
			#else
			vec3 colorCorrect(vec3 c) { return c; }
			#endif

			#if defined(SRGB_SCENE)

			// With sRGB textures the hardware hands the shaders linear values, so
			// the scene buffer holds light rather than display colours and has to
			// be encoded before anything looks at it. Deliberately the plain 2.2
			// power curve and not the piecewise sRGB one: the latter needs the
			// value clamped to a display range, and overbright pixels above one
			// are what the HDR and bloom paths are looking for.
			vec3 encodeDisplay(vec3 c)
			{
				return pow(max(c, 0.0), vec3(1.0 / 2.2));
			}

			#endif

			#if defined(HDR10_MODE)

			// HDR10: Rec.2020 primaries and the ST.2084 (PQ) transfer curve.
			//
			// The scRGB path above hands the compositor linear light and lets it
			// do the display encode. HDR10 has no such arrangement - a code value
			// means a specific luminance, full stop - so both halves of the encode
			// are ours to do here. This is the path Wayland takes, since it offers
			// HDR10 and not scRGB.

			// Rec.709 -> Rec.2020. Both are D65, so this is a pure gamut rotation
			// with no white point adaptation. Skipping it does not fail loudly: the
			// display simply reads our narrow gamut numbers as wide gamut ones and
			// every colour comes back oversaturated.
			vec3 rec709ToRec2020(vec3 c)
			{
				const mat3 M = mat3(
					0.6274040, 0.0690970, 0.0163916,   // column major
					0.3292820, 0.9195400, 0.0880132,
					0.0433136, 0.0113612, 0.8955950);
				return M * c;
			}

			// ST.2084 inverse EOTF. Input is luminance with 1.0 meaning PQ's
			// ceiling of 10000 nits; output is the code value the display decodes.
			vec3 pqEncode(vec3 L)
			{
				const float m1 = 0.1593017578125;   // 2610/16384
				const float m2 = 78.84375;          // 2523/4096 * 128
				const float c1 = 0.8359375;         // 3424/4096
				const float c2 = 18.8515625;        // 2413/4096 * 32
				const float c3 = 18.6875;           // 2392/4096 * 32

				vec3 y = pow(clamp(L, 0.0, 1.0), vec3(m1));
				return pow((c1 + c2 * y) / (1.0 + c3 * y), vec3(m2));
			}

			vec3 encodeHdr10(vec3 c)
			{
				// linearHdr() produces scRGB units, in which 1.0 is 80 nits by
				// definition. Multiplying by that turns them into real luminance,
				// which is what PQ wants and the reason HdrScale means the same
				// brightness in both HDR paths rather than two different ones.
				vec3 nits = linearHdr(max(c, vec3(0.0))) * 80.0;
				return pqEncode(max(rec709ToRec2020(nits), vec3(0.0)) / 10000.0);
			}

			#endif

			void main()
			{
				vec3 sceneColor = texture(texSampler, texCoord).rgb;
			#if defined(SRGB_SCENE)
				sceneColor = encodeDisplay(sceneColor);
			#endif
				vec3 color = gammaCorrect(colorCorrect(sceneColor));
			#if defined(HDR10_MODE)
				outColor = vec4(encodeHdr10(color), 1.0f);
			#elif defined(HDR_MODE)
				outColor = vec4(linearHdr(color), 1.0f);
			#else
				outColor = vec4(dither(color), 1.0f);
			#endif
			}
		)";
	}
	else if (filename == "shaders/BloomExtract.frag")
	{
		return R"(
			layout(push_constant) uniform BloomPushConstants
			{
				float SampleWeights0;
				float SampleWeights1;
				float SampleWeights2;
				float SampleWeights3;
				float SampleWeights4;
				float SampleWeights5;
				float SampleWeights6;
				float SampleWeights7;
			};

			layout(binding = 0) uniform sampler2D texSampler;
			layout(location = 0) in vec2 texCoord;
			layout(location = 0) out vec4 outColor;

			void main()
			{
				outColor = vec4(max(texture(texSampler, texCoord).rgb - 1.0, 0.0), 0.0);
			}
		)";
	}
	else if (filename == "shaders/BloomCombine.frag")
	{
		return R"(
			layout(push_constant) uniform BloomPushConstants
			{
				float SampleWeights0;
				float SampleWeights1;
				float SampleWeights2;
				float SampleWeights3;
				float SampleWeights4;
				float SampleWeights5;
				float SampleWeights6;
				float SampleWeights7;
			};

			layout(binding = 0) uniform sampler2D texSampler;
			layout(location = 0) in vec2 texCoord;
			layout(location = 0) out vec4 outColor;

			void main()
			{
				outColor = texture(texSampler, texCoord);
			}
		)";
	}
	else if (filename == "shaders/Blur.frag")
	{
		return R"(
			layout(push_constant) uniform BloomPushConstants
			{
				float SampleWeights0;
				float SampleWeights1;
				float SampleWeights2;
				float SampleWeights3;
				float SampleWeights4;
				float SampleWeights5;
				float SampleWeights6;
				float SampleWeights7;
			};

			layout(binding = 0) uniform sampler2D texSampler;
			layout(location = 0) in vec2 texCoord;
			layout(location = 0) out vec4 outColor;

			void main()
			{
			#if defined(BLUR_HORIZONTAL)
				outColor =
					textureOffset(texSampler, texCoord, ivec2( 0, 0)) * SampleWeights0 +
					textureOffset(texSampler, texCoord, ivec2( 1, 0)) * SampleWeights1 +
					textureOffset(texSampler, texCoord, ivec2(-1, 0)) * SampleWeights2 +
					textureOffset(texSampler, texCoord, ivec2( 2, 0)) * SampleWeights3 +
					textureOffset(texSampler, texCoord, ivec2(-2, 0)) * SampleWeights4 +
					textureOffset(texSampler, texCoord, ivec2( 3, 0)) * SampleWeights5 +
					textureOffset(texSampler, texCoord, ivec2(-3, 0)) * SampleWeights6;
			#else
				outColor =
					textureOffset(texSampler, texCoord, ivec2(0, 0)) * SampleWeights0 +
					textureOffset(texSampler, texCoord, ivec2(0, 1)) * SampleWeights1 +
					textureOffset(texSampler, texCoord, ivec2(0,-1)) * SampleWeights2 +
					textureOffset(texSampler, texCoord, ivec2(0, 2)) * SampleWeights3 +
					textureOffset(texSampler, texCoord, ivec2(0,-2)) * SampleWeights4 +
					textureOffset(texSampler, texCoord, ivec2(0, 3)) * SampleWeights5 +
					textureOffset(texSampler, texCoord, ivec2(0,-3)) * SampleWeights6;
			#endif
			}
		)";
	}

	return {};
}
