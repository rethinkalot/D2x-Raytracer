// Included inside RenderBackend.cpp's private namespace, after CompileShader.
struct HybridVisibilityState {
    ID3D12PipelineState* pipeline[2]={};
    ID3D12Resource* depth=nullptr;
    DescriptorAllocation rtv={},dsv={};
    uint32_t width=0,height=0;
};
HybridVisibilityState hybridState;
void HybridVisibilityShutdown() {
    for(auto& pipeline:hybridState.pipeline) SafeRelease(pipeline);
    if(hybridState.depth) RT_RELEASE_RESOURCE(hybridState.depth);
    hybridState={};
}
void EnsureHybridVisibility(uint32_t width,uint32_t height) {
    if(!hybridState.pipeline[0]) {
        IDxcBlob* vs=CompileShader(L"assets/shaders/hybrid_visibility.hlsl",L"HybridVS",L"vs_6_5");
        IDxcBlob* ps=CompileShader(L"assets/shaders/hybrid_visibility.hlsl",L"HybridPS",L"ps_6_5");
        RT_ASSERT(vs && ps);
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc={};
        desc.pRootSignature=g_d3d.global_root_sig;
        desc.VS={vs->GetBufferPointer(),vs->GetBufferSize()};
        desc.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
        desc.SampleMask=UINT_MAX; desc.SampleDesc.Count=1;
        desc.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets=1; desc.RTVFormats[0]=DXGI_FORMAT_R32G32_UINT;
        auto& blend=desc.BlendState.RenderTarget[0];
        blend.SrcBlend=D3D12_BLEND_ONE; blend.DestBlend=D3D12_BLEND_ZERO;
        blend.BlendOp=D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha=D3D12_BLEND_ONE; blend.DestBlendAlpha=D3D12_BLEND_ZERO;
        blend.BlendOpAlpha=D3D12_BLEND_OP_ADD; blend.LogicOp=D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.DepthStencilState.DepthEnable=TRUE;
        desc.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;
        desc.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS;
        desc.DSVFormat=DXGI_FORMAT_D32_FLOAT;
        desc.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode=D3D12_CULL_MODE_BACK;
        desc.RasterizerState.DepthClipEnable=TRUE;
        for(int reverse=0;reverse<2;++reverse) {
            desc.RasterizerState.FrontCounterClockwise=reverse ? FALSE : TRUE;
            DX_CALL(g_d3d.device->CreateGraphicsPipelineState(&desc,IID_PPV_ARGS(&hybridState.pipeline[reverse])));
        }
        vs->Release(); ps->Release();
        hybridState.rtv=g_d3d.rtv.Allocate(1);
        hybridState.dsv=g_d3d.dsv.Allocate(1);
    }
    if(hybridState.width!=width || hybridState.height!=height) {
        g_d3d.command_queue_direct->Flush();
        if(hybridState.depth) RT_RELEASE_RESOURCE(hybridState.depth);
        D3D12_CLEAR_VALUE clear={}; clear.Format=DXGI_FORMAT_D32_FLOAT; clear.DepthStencil.Depth=1;
        hybridState.depth=RT_CreateTexture(L"DMAX 11 VR visibility depth",DXGI_FORMAT_D32_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,width,height,D3D12_RESOURCE_STATE_DEPTH_WRITE,1,&clear);
        CreateTextureDSV(hybridState.depth,hybridState.dsv.cpu,DXGI_FORMAT_D32_FLOAT);
        hybridState.width=width; hybridState.height=height;
    }
}
void RasterizeHybridVisibility(CommandList& commands,FrameData* frame,D3D12RenderTargets& rt,uint32_t width,uint32_t height) {
    EnsureHybridVisibility(width,height);
    ID3D12DescriptorHeap* heaps[]={g_d3d.cbv_srv_uav.GetHeap()};
    commands->SetDescriptorHeaps(1,heaps);
    ResourceTransition(commands,rt.visibility_prim,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const UINT empty[4]={UINT_MAX,UINT_MAX,UINT_MAX,UINT_MAX};
    commands->ClearUnorderedAccessViewUint(frame->descriptors.GetGPUDescriptor(D3D12GlobalDescriptors_UAV_visibility_prim),
        frame->non_shader_descriptors.GetCPUDescriptor(D3D12GlobalDescriptors_UAV_visibility_prim),rt.visibility_prim,empty,0,nullptr);
    UAVBarrier(commands,rt.visibility_prim);
    ResourceTransition(commands,rt.visibility_prim,D3D12_RESOURCE_STATE_RENDER_TARGET);
    CreateTextureRTV(rt.visibility_prim,hybridState.rtv.cpu,DXGI_FORMAT_R32G32_UINT);
    ResourceTransition(commands,hybridState.depth,D3D12_RESOURCE_STATE_DEPTH_WRITE);
    D3D12_VIEWPORT viewport={0,0,static_cast<float>(width),static_cast<float>(height),0,1};
    D3D12_RECT scissor={0,0,static_cast<LONG>(width),static_cast<LONG>(height)};
    commands->RSSetViewports(1,&viewport); commands->RSSetScissorRects(1,&scissor);
    commands->OMSetRenderTargets(1,&hybridState.rtv.cpu,FALSE,&hybridState.dsv.cpu);
    commands->SetGraphicsRootSignature(g_d3d.global_root_sig);
    commands->SetGraphicsRootDescriptorTable(RaytraceRootParameters_MainDescriptorTable,frame->descriptors.gpu);
    commands->SetGraphicsRootDescriptorTable(RaytraceRootParameters_BindlessSRVTable,g_d3d.cbv_srv_uav.GetGPUBase());
    commands->SetGraphicsRootDescriptorTable(RaytraceRootParameters_BindlessTriangleBufferTable,g_d3d.cbv_srv_uav.GetGPUBase());
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    auto* instances=frame->instance_data.As<InstanceData>();
    auto* descriptions=frame->instance_descs.As<D3D12_RAYTRACING_INSTANCE_DESC>();
    // Clearing only depth between layers keeps the cockpit in the foreground
    // while preserving world pixels through transparent parts and windows.
    for(int layer=0;layer<2;++layer) {
        commands->ClearDepthStencilView(hybridState.dsv.cpu,D3D12_CLEAR_FLAG_DEPTH,1,0,0,nullptr);
        for(UINT instance=0;instance<g_d3d.tlas_instance_count;++instance) {
            const UINT mask=instances[instance].instance_mask;
            const bool cockpit=(mask & RT_RENDER_MASK_COCKPIT)!=0;
            if(cockpit!=(layer==1) || !(mask & ~RT_RENDER_MASK_PORTALS)) continue;
            const auto& m=instances[instance].object_to_world.e;
            const float determinant=m[0][0]*(m[1][1]*m[2][2]-m[1][2]*m[2][1])-
                m[0][1]*(m[1][0]*m[2][2]-m[1][2]*m[2][0])+m[0][2]*(m[1][0]*m[2][1]-m[1][1]*m[2][0]);
            const bool reverse=(descriptions[instance].Flags & D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE)!=0;
            // DXR classifies faces in object space. Rasterization classifies
            // projected winding, so reflected object transforms need a flip.
            const int winding=(reverse!=(determinant>=0)) ? 1 : 0;
            commands->SetPipelineState(hybridState.pipeline[winding]);
            commands->SetGraphicsRoot32BitConstant(RaytraceRootParameters_DenoiseIteration,instance,0);
            commands->DrawInstanced(frame->raster_vertex_counts[instance],1,0,0);
        }
    }
    commands->OMSetRenderTargets(0,nullptr,FALSE,nullptr);
    ResourceTransition(commands,rt.visibility_prim,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}
