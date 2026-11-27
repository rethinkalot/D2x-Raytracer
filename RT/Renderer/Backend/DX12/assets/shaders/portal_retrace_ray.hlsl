#ifndef PORTAL_RETRACE_RAY_HLSL
#define PORTAL_RETRACE_RAY_HLSL

#include "include/common.hlsl"

struct PortalRetraceRayPayload
{
	int search_segment;
	bool found;
	float hit_distance;
	int next_segment;
};

void TracePortalRetraceRay(RayDesc ray, inout PortalRetraceRayPayload payload, uint2 pixel_pos, bool use_level_geo)
{
#if RT_INLINE_RAYTRACING
	uint mask = use_level_geo ? (1 | 2) : 2;
	RayQuery<RAY_FLAG_CULL_BACK_FACING_TRIANGLES> ray_query;
	ray_query.TraceRayInline(g_scene,
		RAY_FLAG_CULL_BACK_FACING_TRIANGLES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
		mask, ray);

	while (ray_query.Proceed())
	{
		if (ray_query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
		{
			uint instance_idx = ray_query.CandidateInstanceIndex();
			uint primitive_idx = ray_query.CandidatePrimitiveIndex();
			InstanceData instance_data = g_instance_data_buffer[instance_idx];
			RT_Triangle hit_triangle = GetHitTriangle(instance_data.triangle_buffer_idx, primitive_idx);

			if (hit_triangle.segment == payload.search_segment)
			{
				payload.found = true;
				payload.hit_distance = ray_query.CandidateTriangleRayT();
				payload.next_segment = hit_triangle.segment_adjacent;
				ray_query.CommitNonOpaqueTriangleHit();
			}
		}
	}
#endif
}

#endif
