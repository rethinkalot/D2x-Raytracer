// Since DirectX 12 does not have a vkCmdBlitImage equivalent,
// we have to make do with a quad-based blit technique
// ------------------------------------------------------
// Vertex shader

struct Vertex
{
	float2 pos;
	float2 uv;
};

static Vertex g_quad_verts[6] = {
	{ float2(-1.0f, 1.0f), float2(0.0f, 0.0f) },
	{ float2(1.0f, 1.0f), float2(1.0f, 0.0f) },
	{ float2(1.0f, -1.0f), float2(1.0f, 1.0f) },
	{ float2(-1.0f, 1.0f), float2(0.0f, 0.0f) },
	{ float2(1.0f, -1.0f), float2(1.0f, 1.0f) },
	{ float2(-1.0f, -1.0f), float2(0.0f, 1.0f) }
};

struct Constants
{
	float blit_x;        // offset within viewport (pixels)
	float blit_y;        // offset within viewport (pixels)
	float blit_width;    // draw width in pixels
	float blit_height;   // draw height in pixels
	float blit_blend;    // blend flag (0/1)
	float screen_width;  // viewport width in pixels
	float screen_height; // viewport height in pixels
};

ConstantBuffer<Constants> g_constants : register(b0);

struct VertexShaderOutput
{
	float4 position : SV_POSITION;
	float2 uv : TEX_COORDS;
	float2 pixelPos : TEX_COORD1; // pixel-space position inside viewport
};

VertexShaderOutput VertexShaderEntry(uint vertex_id : SV_VertexID)
{
	VertexShaderOutput OUT;
	// compute pixel-space position within the blit rectangle
	float2 quad01 = g_quad_verts[vertex_id].pos * 0.5f + 0.5f; // 0..1
	float2 pixelPos = float2(quad01.x * g_constants.blit_width + g_constants.blit_x,
							 (1.0f - quad01.y) * g_constants.blit_height + g_constants.blit_y);
	OUT.pixelPos = pixelPos;
	OUT.uv = g_quad_verts[vertex_id].uv;

	// convert pixelPos (viewport-local) to NDC clip space (-1..1)
	float ndc_x = (pixelPos.x / g_constants.screen_width) * 2.0f - 1.0f;
	float ndc_y = 1.0f - (pixelPos.y / g_constants.screen_height) * 2.0f; // invert Y
	OUT.position = float4(ndc_x, ndc_y, 0.0f, 1.0f);
	return OUT;
}

// ------------------------------------------------------
// Pixel shader

#define BLIT_BLEND_INNER 0.5
#define BLIT_BLEND_OUTER 1.0
#define BLIT_BLEND_STRENGTH 1.0
// Shape will make the blend form more circular with higher values (vignette)
#define BLIT_BLEND_SHAPE 0.5

Texture2D g_source_texture : register(t0);
SamplerState g_sampler : register(s0);

float4 PixelShaderEntry(VertexShaderOutput IN) : SV_Target
{
	float blend_factor = 1.0;
	// Compute uv mapping from pixel position (provided by vertex shader) into the blit rect
	float2 uv = (IN.pixelPos - float2(g_constants.blit_x, g_constants.blit_y)) / float2(max(1.0f, g_constants.blit_width), max(1.0f, g_constants.blit_height));
	// Ensure uv is valid before use
	uv = clamp(uv, 0.0f, 1.0f);
	if (g_constants.blit_blend)
	{
		float2 blend_curve = pow(abs(uv * 2.0 - 1.0), float2(1.0 / BLIT_BLEND_SHAPE, 1.0 / BLIT_BLEND_SHAPE));
		float blend_edge = pow(length(blend_curve), BLIT_BLEND_SHAPE);
		blend_factor = 1.0 - BLIT_BLEND_STRENGTH * smoothstep(BLIT_BLEND_INNER, BLIT_BLEND_OUTER, blend_edge);
	}

	// Sample using computed 'uv' that maps screen position into the blit rectangle
	float4 sampled_color = g_source_texture.SampleLevel(g_sampler, uv, 0);
	return float4(sampled_color.xyz, blend_factor);
}
