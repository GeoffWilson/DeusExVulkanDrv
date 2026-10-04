#pragma once

#include "vec.h"
#include <cmath>

// The seat a headset's room is fitted to, and turning between it and the
// world, for the helper - which places the eyes and the HUD's panels in it -
// and the render device - which has the game project its HUD through where
// the head looks in it.
//
// A view is as the trace takes one (TraceProtocol's Camera): the eye, then
// right, down the picture and ahead. The seat's space is OpenXR's: x right, y
// up, z back.
namespace HeadsetSeat
{
	// A direction in a quaternion's (x, y, z, w) own terms, turned into the
	// space it is given in.
	inline vec3 Turn(const float* q, const vec3& v)
	{
		const vec3 u(q[0], q[1], q[2]);
		const vec3 uv = cross(u, v);
		return v + (uv * q[3] + cross(u, uv)) * 2.0f;
	}

	// The seat: facing the way the view faces, but level, whatever that
	// view's pitch and roll. Looking up and down with the mouse would
	// otherwise tilt the whole world in the headset, which is the surest way
	// to make its wearer ill.
	//
	// Ahead is the view's heading; looking straight down it is the way the
	// top of the picture points, and straight up the way its foot does. Right
	// is taken to the same side of it as the view's right is, whichever way
	// round the world's axes turn.
	inline void Axes(const vec3& r, const vec3& d, const vec3& f, vec3& right, vec3& down, vec3& ahead)
	{
		const vec3 up(0.0f, 0.0f, 1.0f);
		vec3 heading(f.x, f.y, 0.0f);
		if (length(heading) < 1.0e-3f)
			heading = f.z < 0.0f ? vec3(-d.x, -d.y, 0.0f) : vec3(d.x, d.y, 0.0f);
		ahead = length(heading) > 1.0e-6f ? normalize(heading) : vec3(1.0f, 0.0f, 0.0f);
		const float handed = dot(cross(r, d), f) < 0.0f ? -1.0f : 1.0f;
		right = cross(ahead, up) * handed;
		down = vec3(0.0f, 0.0f, -1.0f);
	}

	// A direction in the seat's space, in the world's, and back.
	inline vec3 ToWorld(const vec3& right, const vec3& down, const vec3& ahead, const vec3& v)
	{
		return right * v.x - down * v.y - ahead * v.z;
	}
	inline vec3 ToSeat(const vec3& right, const vec3& down, const vec3& ahead, const vec3& v)
	{
		return vec3(dot(v, right), -dot(v, down), -dot(v, ahead));
	}

	// The rotation whose columns are x, y and z, as a quaternion (x, y, z, w).
	inline void Quaternion(const vec3& x, const vec3& y, const vec3& z, float* q)
	{
		const float m00 = x.x, m10 = x.y, m20 = x.z, m01 = y.x, m11 = y.y, m21 = y.z, m02 = z.x, m12 = z.y, m22 = z.z;
		const float trace = m00 + m11 + m22;
		if (trace > 0.0f)
		{
			const float s = std::sqrt(trace + 1.0f) * 2.0f;
			q[3] = 0.25f * s; q[0] = (m21 - m12) / s; q[1] = (m02 - m20) / s; q[2] = (m10 - m01) / s;
		}
		else if (m00 > m11 && m00 > m22)
		{
			const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
			q[3] = (m21 - m12) / s; q[0] = 0.25f * s; q[1] = (m01 + m10) / s; q[2] = (m02 + m20) / s;
		}
		else if (m11 > m22)
		{
			const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
			q[3] = (m02 - m20) / s; q[0] = (m01 + m10) / s; q[1] = 0.25f * s; q[2] = (m12 + m21) / s;
		}
		else
		{
			const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
			q[3] = (m10 - m01) / s; q[0] = (m02 + m20) / s; q[1] = (m12 + m21) / s; q[2] = 0.25f * s;
		}
		const float l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
		for (int i = 0; i < 4; i++)
			q[i] /= l;
	}

	// Which way a view faces from the seat fitted to it, as a quaternion: the
	// orientation of a panel spanning it, the panel's own z towards the eye.
	inline void ViewInSeat(const vec3& r, const vec3& d, const vec3& f, float* q)
	{
		vec3 right, down, ahead;
		Axes(r, d, f, right, down, ahead);
		const vec3 x = ToSeat(right, down, ahead, normalize(r));
		const vec3 y = ToSeat(right, down, ahead, normalize(d) * -1.0f);
		const vec3 z = ToSeat(right, down, ahead, normalize(f) * -1.0f);
		Quaternion(x, y, z, q);
	}

	// The view a head looks along from the seat fitted to the view r, d, f -
	// its right, down and ahead in the world - for the HUD to be projected
	// through where the head looks rather than where the mouse aims.
	inline void HeadView(const vec3& r, const vec3& d, const vec3& f, const float* head, vec3& headRight, vec3& headDown, vec3& headAhead)
	{
		vec3 right, down, ahead;
		Axes(r, d, f, right, down, ahead);
		headRight = ToWorld(right, down, ahead, Turn(head, vec3(1.0f, 0.0f, 0.0f)));
		headDown = ToWorld(right, down, ahead, Turn(head, vec3(0.0f, -1.0f, 0.0f)));
		headAhead = ToWorld(right, down, ahead, Turn(head, vec3(0.0f, 0.0f, -1.0f)));
	}
}
