struct CViewProj { float4x4 mat; };
struct CFarPlane { float plane; };
ConstantBuffer<CViewProj> c_view_projection : register(b0);
ConstantBuffer<CFarPlane> c_far_plane : register(b1);

struct VertexInput
{
	float3 Position : POSITION;
	float2 TexCoord : TEXCOORD;
	float4 Color : COLOR;
	uint TextureIndex : TEXTURE_INDEX;
};

struct VertexOutput
{
	float4 Position : SV_POSITION;
	float2 TexCoord : TEXCOORD;
	float4 Color : COLOR;
	uint TextureIndex : TEXTURE_INDEX;
};

VertexOutput VertexShaderEntry(VertexInput IN)
{
	VertexOutput OUT;
	OUT.Position = mul(c_view_projection.mat, float4(IN.Position, 1.0));
	OUT.TexCoord = IN.TexCoord;
	OUT.Color = IN.Color;
	OUT.TextureIndex = IN.TextureIndex;
	return OUT;
}

Texture2D g_textures[] : register(t0);
SamplerState g_sampler : register(s0);

struct PixelOutput
{
	float Depth : SV_DEPTH;
	float4 Color : SV_TARGET;
};

PixelOutput PixelShaderEntry(VertexOutput IN)
{
	PixelOutput OUT;
	float4 texel = g_textures[NonUniformResourceIndex(IN.TextureIndex)].Sample(g_sampler, IN.TexCoord);
	clip(texel.a - 0.01);
	OUT.Depth = (IN.Position.z / c_far_plane.plane) * IN.Position.w;
	OUT.Color = float4(texel.rgb * IN.Color.rgb, texel.a * IN.Color.a);
	return OUT;
}
