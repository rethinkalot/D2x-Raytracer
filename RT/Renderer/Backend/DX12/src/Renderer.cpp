#include "mikktspace.h"

//
//
//

#include "Renderer.h"
#include "RenderBackend.h"
#ifdef D2_ENABLE_OPENVR
#include "OpenVRPrototype.h"
#endif
#include "Core/Common.h"
#include "Core/Arena.h"

//
//
//

void RT_RendererInit(const RT_RendererInitParams* renderer_init_params)
{
	RenderBackend::Init(renderer_init_params);
#ifdef D2_ENABLE_OPENVR
	OpenVRPrototype::Init();
#endif
}

void RT_RendererExit(void)
{
#ifdef D2_ENABLE_OPENVR
	OpenVRPrototype::Shutdown();
#endif
	RenderBackend::Exit();
}

void RT_BeginFrame(void)
{
	RenderBackend::BeginFrame();
#ifdef D2_ENABLE_OPENVR
	OpenVRPrototype::BeginFrame();
#endif
}

void RT_BeginScene(const RT_SceneSettings* scene_settings)
{
	RenderBackend::BeginScene(scene_settings);
}

void RT_VRBeginReticle(RT_Vec3 target) {
#ifdef D2_ENABLE_OPENVR
    OpenVRPrototype::BeginReticle(target.x,target.y,target.z);
#else
    (void)target;
#endif
}
void RT_VREndReticle(void) {
#ifdef D2_ENABLE_OPENVR
    OpenVRPrototype::EndReticle();
#endif
}

RT_VRGraphicsState RT_VRGetGraphicsState(void) {
#ifdef D2_ENABLE_OPENVR
    return OpenVRPrototype::GraphicsState();
#else
    return {};
#endif
}
bool RT_VRSaveGraphicsSettings(const RT_VRGraphicsSettings* settings) {
#ifdef D2_ENABLE_OPENVR
    return OpenVRPrototype::SaveGraphicsSettings(settings);
#else
    (void)settings;return false;
#endif
}
bool RT_VRActive(void)
{
#ifdef D2_ENABLE_OPENVR
	return OpenVRPrototype::Active();
#else
	return false;
#endif
}

bool RT_VRCompositorPaced(void)
{
#ifdef D2_ENABLE_OPENVR
	return OpenVRPrototype::CompositorPaced();
#else
	return false;
#endif
}

int RT_BeginCameraView(int view)
{
	return RenderBackend::BeginCameraView(view);
}

void RT_EndCameraView(void)
{
	RenderBackend::EndCameraView();
}

float RT_VRObjectScale(void) {
#ifdef D2_ENABLE_OPENVR
    return OpenVRPrototype::ObjectScale();
#else
    return 1.0f;
#endif
}

void RT_EndScene(void)
{
#ifdef D2_ENABLE_OPENVR
	if (OpenVRPrototype::RenderScene()) return;
#endif
	RenderBackend::EndScene();
	RenderBackend::RaytraceRender();
	RenderBackend::RasterRenderDebugLines();
}

void RT_EndFrame(void)
{
	RenderBackend::EndFrame();
}

void RT_SwapBuffers(void)
{
	RenderBackend::SwapBuffers();
}

RT_MaterialEdge *RT_GetMaterialEdgesArray(void)
{
	return g_rt_material_edges;
}

uint16_t *RT_GetMaterialIndicesArray(void)
{
	return g_rt_material_indices;
}

RT_RendererIO *RT_GetRendererIO(void)
{
	return RenderBackend::GetIO();
}

void RT_DoRendererDebugMenus(const RT_DoRendererDebugMenuParams *params)
{
	RenderBackend::DoDebugMenus(params);
}

RT_ResourceHandle RT_UploadTexture(const RT_UploadTextureParams* params)
{
	return RenderBackend::UploadTexture(*params);
}

#if 0
void RT_UploadTextureData(const RT_ResourceHandle texture_handle, const RT_UploadTextureParams* params)
{
	RenderBackend::BackendUploadTextureData(texture_handle, *params);
}
#endif

RT_ResourceHandle RT_GetDefaultWhiteTexture(void)
{
	return RenderBackend::GetDefaultWhiteTexture();
}

RT_ResourceHandle RT_GetDefaultBlackTexture(void)
{
	return RenderBackend::GetDefaultBlackTexture();
}

RT_ResourceHandle RT_GetBillboardMesh(void)
{
	return RenderBackend::GetBillboardMesh();
}

RT_ResourceHandle RT_GetCubeMesh(void)
{
	return RenderBackend::GetCubeMesh();
}

int RT_CheckWindowMinimized(void) {
	return RenderBackend::CheckWindowMinimized();
}

uint16_t RT_UpdateMaterial(uint16_t material_index, const RT_Material *material)
{
	return RenderBackend::UpdateMaterial(material_index, material);
}

bool RT_GenerateTangents(RT_Triangle *triangles, size_t triangle_count)
{
#if 1
	if (NEVER(triangle_count > INT32_MAX))
	{
		return false;
	}

	struct Triangles
	{
		RT_Triangle *triangles;
		size_t triangle_count;
	} triangle_context = { triangles, triangle_count };

	SMikkTSpaceInterface interface = {};
	interface.m_getNumFaces = [](const SMikkTSpaceContext *context) -> int
	{
		Triangles *tri = (Triangles *)context->m_pUserData;
		return (int)tri->triangle_count;
	};

	interface.m_getNumVerticesOfFace = [](const SMikkTSpaceContext *context, const int face) -> int
	{
		(void)context;
		(void)face;
		return 3;
	};

	interface.m_getPosition = [](const SMikkTSpaceContext *context, float pos_out[], const int face, const int vert)
	{
		Triangles *tri = (Triangles *)context->m_pUserData;
		RT_Vec3 *pos = &tri->triangles[face].positions[vert];
		pos_out[0] = pos->x;
		pos_out[1] = pos->y;
		pos_out[2] = pos->z;
	};

	interface.m_getNormal = [](const SMikkTSpaceContext *context, float normal_out[], const int face, const int vert)
	{
		Triangles *tri = (Triangles *)context->m_pUserData;
		RT_Vec3 *normal = &tri->triangles[face].normals[vert];
		normal_out[0] = normal->x;
		normal_out[1] = normal->y;
		normal_out[2] = normal->z;
	};

	interface.m_getTexCoord = [](const SMikkTSpaceContext *context, float texcoord_out[], const int face, const int vert)
	{
		Triangles *tri = (Triangles *)context->m_pUserData;
		RT_Vec2 *uv = &tri->triangles[face].uvs[vert];
		texcoord_out[0] = uv->x;
		texcoord_out[1] = uv->y;
	};

	interface.m_setTSpaceBasic = [](const SMikkTSpaceContext *context, const float result_tangent[], const float sign, const int face, const int vert)
	{
		Triangles *tri = (Triangles *)context->m_pUserData;

		RT_Vec4 *tangent = &tri->triangles[face].tangents[vert];
		tangent->x = result_tangent[0];
		tangent->y = result_tangent[1];
		tangent->z = result_tangent[2];
		tangent->w = -sign; // flip the handedness
	};

	SMikkTSpaceContext mikkt_context = {};
	mikkt_context.m_pInterface = &interface;
	mikkt_context.m_pUserData  = &triangle_context;

	return genTangSpaceDefault(&mikkt_context);
#else
	(void)triangles;
	(void)triangle_count;
	return false;
#endif
}

RT_ResourceHandle RT_UploadMesh(const RT_UploadMeshParams* params)
{
	return RenderBackend::UploadMesh(*params);
}

void RT_ReleaseTexture(const RT_ResourceHandle texture_handle)
{
	RenderBackend::ReleaseTexture(texture_handle);
}

void RT_ReleaseMesh(const RT_ResourceHandle mesh_handle)
{
	RenderBackend::ReleaseMesh(mesh_handle);
}

static uint32_t g_override_flags = 0;

uint32_t RT_RaytraceSetRenderFlagsOverride(uint32_t flags)
{
	uint32_t old_flags = g_override_flags;
	g_override_flags = flags;

	return old_flags;
}

void RT_RaytraceMeshEx(RT_RenderMeshParams* render_mesh_params)
{
    RT_RenderMeshParams params = *render_mesh_params;
    params.flags |= g_override_flags; // override is the wrong name
	if (params.instance_mask == 0)
		params.instance_mask = RT_RENDER_MASK_OBJECTS;

	RenderBackend::RaytraceMesh(params);
}

void RT_RaytraceMeshColor(RT_ResourceHandle mesh, RT_Vec4 color, const RT_Mat4* transform,
    const RT_Mat4* prev_transform) {
    RT_RenderMeshParams params = {};
    params.mesh_handle = mesh;
    params.transform = transform;
    params.prev_transform = prev_transform;
    params.color = RT_PackRGBA(color);

    RT_RaytraceMeshEx(&params);
}

void RT_RaytraceMesh(RT_ResourceHandle mesh, const RT_Mat4* transform, const RT_Mat4* prev_transform) {
    RT_RenderMeshParams params = {};
    params.mesh_handle = mesh;
    params.transform = transform;
    params.prev_transform = prev_transform;
    params.color = 0xFFFFFFFF;

    RT_RaytraceMeshEx(&params);
}

void RT_RaytraceMeshMasked(RT_ResourceHandle mesh, const RT_Mat4* transform, const RT_Mat4* prev_transform, uint32_t instance_mask)
{
	RT_RenderMeshParams params = {};
	params.mesh_handle = mesh;
	params.transform = transform;
	params.prev_transform = prev_transform;
	params.color = 0xFFFFFFFF;
	params.instance_mask = instance_mask;
	RT_RaytraceMeshEx(&params);
}

void RT_RaytraceMeshOverrideMaterial(RT_ResourceHandle mesh, uint16_t material_override, const RT_Mat4* transform, const RT_Mat4* prev_transform)
{
	RT_RenderMeshParams params = {};
	params.mesh_handle       = mesh;
	params.transform         = transform;
	params.prev_transform    = prev_transform;
	params.color             = 0xFFFFFFFF;
	params.material_override = material_override;
	params.flags             = g_override_flags;
    RenderBackend::RaytraceMesh(params);
}

void RT_RaytraceBillboard(uint16_t material_index, RT_Vec2 dim, RT_Vec3 pos, RT_Vec3 prev_pos)
{
	RT_RaytraceBillboardColored(material_index, { 1, 1, 1 }, dim, pos, prev_pos);
}

void RT_RaytraceBillboardColored(uint16_t material_index, RT_Vec3 color, RT_Vec2 dim, RT_Vec3 pos, RT_Vec3 prev_pos)
{
	RenderBackend::RaytraceBillboardColored(material_index, color, dim, pos, prev_pos);
}

void RT_RaytraceBillboardColoredAlpha(uint16_t material_index, RT_Vec3 color, float alpha, RT_Vec2 dim, RT_Vec3 pos, RT_Vec3 prev_pos)
{
	RenderBackend::RaytraceBillboardColoredAlpha(material_index, color, alpha, dim, pos, prev_pos);
}

void RT_RaytraceRod(uint16_t material_index, RT_Vec3 bot_p, RT_Vec3 top_p, float width)
{
	RenderBackend::RaytraceRod(material_index, bot_p, top_p, width);
}

void RT_RaytraceRender()
{
	RenderBackend::RaytraceRender();
}

void RT_RaytraceSubmitLights(size_t light_count, const RT_Light *lights)
{
	RenderBackend::RaytraceSubmitLights(light_count, lights);
}

void RT_RasterSetViewport(float x, float y, float width, float height)
{
	RenderBackend::RasterSetViewport(x, y, width, height);
}

void RT_RasterSetRenderTarget(RT_ResourceHandle texture)
{
	RenderBackend::RasterSetRenderTarget(texture);
}

void RT_RasterSetRenderTargetNoClear(RT_ResourceHandle texture)
{
	RenderBackend::RasterSetRenderTarget(texture, false);
}

uint32_t RT_RaytraceGetCurrentLightCount() {
	return RenderBackend::RaytraceGetCurrentLightCount();
}

uint32_t RT_RaytraceGetCurrentInstanceCount(void) {
	return RenderBackend::RaytraceGetCurrentInstanceCount();
}

uint32_t RT_RaytraceGetInstanceCapacity(void) {
	return RenderBackend::RaytraceGetInstanceCapacity();
}

void RT_RaytraceSetVerticalOffset(const float new_offset) {
    RenderBackend::RaytraceSetVerticalOffset(new_offset);
}

float RT_RaytraceGetVerticalOffset() {
    return RenderBackend::RaytraceGetVerticalOffset();
}

void RT_RaytraceSetSkyColors(const RT_Vec3 sky_top, const RT_Vec3 sky_bottom)
{
	RenderBackend::RaytraceSetSkyColors(sky_top, sky_bottom);
}

void RT_SetShieldEffects(const RT_ShieldEffects* effects)
{
	RenderBackend::SetShieldEffects(effects);
}

void RT_RasterTriangles(RT_RasterTrianglesParams* params, uint32_t num_params)
{
	RenderBackend::RasterTriangles(params, num_params);
}

void RT_RasterLines(RT_RasterLineVertex* vertices, uint32_t num_vertices)
{
	RenderBackend::RasterLines(vertices, num_vertices);
}

void RT_RasterLineWorld(RT_Vec3 a, RT_Vec3 b, RT_Vec4 color)
{
	RT_RasterLineVertex vertices[2];
	vertices[0].pos = a;
	vertices[0].color = color;
	vertices[1].pos = b;
	vertices[1].color = color;

	RT_RasterLinesWorld(vertices, 2);
}

void RT_RasterLinesWorld(RT_RasterLineVertex* vertices, uint32_t num_vertices)
{
	RenderBackend::RasterLinesWorld(vertices, num_vertices);
}

void RT_RasterBlitScene(const RT_Vec2* top_left, const RT_Vec2* bottom_right, bool blit_blend)
{
	RenderBackend::RasterBlitScene(top_left, bottom_right, blit_blend);
}

void RT_RasterBlit(RT_ResourceHandle src, const RT_Vec2* top_left, const RT_Vec2* bottom_right, bool blit_blend)
{
	RenderBackend::RasterBlit(src, top_left, bottom_right, blit_blend);
}

void RT_RasterRender()
{
	RenderBackend::RasterRender();
}

RT_API void RT_RenderImGuiTexture(RT_ResourceHandle texture_handle, float width, float height)
{
	RenderBackend::RenderImGuiTexture(texture_handle, width, height);
}

void RT_RenderImGui(void)
{
	RenderBackend::RenderImGui();
}

void RT_QueueScreenshot(const char *file_name)
{
	RenderBackend::QueueScreenshot(file_name);
}

bool RT_GetRetraceRays()
{
	return RenderBackend::GetRetraceRays();
}

void RT_Flush()
{
	RenderBackend::Flush();
}

void RT_WaitForSharedSceneData(void)
{
	RenderBackend::WaitForSharedSceneData();
}

RT_Arena *RT_ArenaGetThreadArena()
{
	return &g_thread_arena;
}
