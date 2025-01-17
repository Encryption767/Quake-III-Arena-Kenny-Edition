#include "tr_local.h"
#include "../../engine/platform/win_local.h"

#include <chrono>
#include <functional>

#ifdef ENABLE_DX12

#include "D3d12.h"
#include "DXGI1_4.h"

#pragma comment (lib, "D3d12.lib")
#pragma comment (lib, "DXGI.lib")

const int VERTEX_CHUNK_SIZE = 512 * 1024;

const int XYZ_SIZE      = 4 * VERTEX_CHUNK_SIZE;
const int COLOR_SIZE    = 1 * VERTEX_CHUNK_SIZE;
const int ST0_SIZE      = 2 * VERTEX_CHUNK_SIZE;
const int ST1_SIZE      = 2 * VERTEX_CHUNK_SIZE;

const int XYZ_OFFSET    = 0;
const int COLOR_OFFSET  = XYZ_OFFSET + XYZ_SIZE;
const int ST0_OFFSET    = COLOR_OFFSET + COLOR_SIZE;
const int ST1_OFFSET    = ST0_OFFSET + ST0_SIZE;

const int VERTEX_BUFFER_SIZE = XYZ_SIZE + COLOR_SIZE + ST0_SIZE + ST1_SIZE;
const int INDEX_BUFFER_SIZE = 2 * 1024 * 1024;

#define DX_CHECK(function_call) { \
	HRESULT hr = function_call; \
	if (FAILED(hr)) \
		ri.Error(ERR_FATAL, "Direct3D: error returned by %s", #function_call); \
}

static DXGI_FORMAT get_depth_format() {
	if (r_stencilbits->integer > 0) {
		glConfig.stencilBits = 8;
		return DXGI_FORMAT_D24_UNORM_S8_UINT;
	} else {
		glConfig.stencilBits = 0;
		return DXGI_FORMAT_D32_FLOAT;
	}
}

static void get_hardware_adapter(IDXGIFactory4* factory, IDXGIAdapter1** adapter) {
	DXGI_ADAPTER_DESC1 desc;
	UINT adapter_index = 0;
	while (factory->EnumAdapters1(adapter_index++, adapter) != DXGI_ERROR_NOT_FOUND) {
		(*adapter)->GetDesc1(&desc);
		if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
			continue;
		}
		// check for 11_0 feature level support
		if (SUCCEEDED(D3D12CreateDevice(*adapter, D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr))) {
			return;
		}
	}
	*adapter = nullptr;
}

static void record_and_run_commands(std::function<void(ID3D12GraphicsCommandList*)> recorder) {
	ID3D12GraphicsCommandList* command_list;
	DX_CHECK(dx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, dx.helper_command_allocator,
		nullptr, IID_PPV_ARGS(&command_list)));

	recorder(command_list);
	DX_CHECK(command_list->Close());

	ID3D12CommandList* command_lists[] = { command_list };
	dx.command_queue->ExecuteCommandLists(1, command_lists);
	dx_wait_device_idle();
	
	command_list->Release();
	dx.helper_command_allocator->Reset();
}

static D3D12_RESOURCE_BARRIER get_transition_barrier(
	ID3D12Resource* resource,
	D3D12_RESOURCE_STATES state_before,
	D3D12_RESOURCE_STATES state_after)
{
	D3D12_RESOURCE_BARRIER barrier;
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = resource;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = state_before;
	barrier.Transition.StateAfter = state_after;
	return barrier;
}

static D3D12_HEAP_PROPERTIES get_heap_properties(D3D12_HEAP_TYPE heap_type) {
	D3D12_HEAP_PROPERTIES properties;
	properties.Type = heap_type;
	properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	properties.CreationNodeMask = 1;
	properties.VisibleNodeMask = 1;
	return properties;
}

static D3D12_RESOURCE_DESC get_buffer_desc(UINT64 size) {
	D3D12_RESOURCE_DESC desc;
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Alignment = 0;
	desc.Width = size;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_UNKNOWN;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	desc.Flags = D3D12_RESOURCE_FLAG_NONE;
	return desc;
}

ID3D12PipelineState* create_pipeline(const Vk_Pipeline_Def& def);

void dx_initialize() {
	// enable validation in debug configuration
#if defined(_DEBUG)
	ID3D12Debug* debug_controller;
	if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_controller)))) {
		debug_controller->EnableDebugLayer();
		debug_controller->Release();
	}
#endif

	IDXGIFactory4* factory;
	DX_CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))

	// Create device.
	{
		IDXGIAdapter1* hardware_adapter;
		get_hardware_adapter(factory, &hardware_adapter);
		DX_CHECK(D3D12CreateDevice(hardware_adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dx.device)))
		hardware_adapter->Release();
	}

	// Create command queue.
	{
		D3D12_COMMAND_QUEUE_DESC queue_desc{};
		queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
		DX_CHECK(dx.device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&dx.command_queue)))
	}

	//
	// Create swap chain.
	//
	{
		DXGI_SWAP_CHAIN_DESC1 swap_chain_desc{};
		swap_chain_desc.BufferCount = SWAPCHAIN_BUFFER_COUNT;
		swap_chain_desc.Width = glConfig.vidWidth;
		swap_chain_desc.Height = glConfig.vidHeight;
		swap_chain_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		swap_chain_desc.SampleDesc.Count = 1;

		IDXGISwapChain1* swapchain;
		DX_CHECK(factory->CreateSwapChainForHwnd(
			dx.command_queue,
			g_wv.hWnd_dx,
			&swap_chain_desc,
			nullptr,
			nullptr,
			&swapchain
			))

		DX_CHECK(factory->MakeWindowAssociation(g_wv.hWnd_dx, DXGI_MWA_NO_ALT_ENTER))
		swapchain->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&dx.swapchain);
		swapchain->Release();

		for (int i = 0; i < SWAPCHAIN_BUFFER_COUNT; i++) {
			DX_CHECK(dx.swapchain->GetBuffer(i, IID_PPV_ARGS(&dx.render_targets[i])))
		}
	}

	factory->Release();
	factory = nullptr;

	//
	// Create command allocators and command list.
	//
	{
		DX_CHECK(dx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
			IID_PPV_ARGS(&dx.command_allocator)))

		DX_CHECK(dx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
			IID_PPV_ARGS(&dx.helper_command_allocator)))

		DX_CHECK(dx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, dx.command_allocator, nullptr,
			IID_PPV_ARGS(&dx.command_list)));
		DX_CHECK(dx.command_list->Close())
	}

	//
	// Create synchronization objects.
	//
	{
		DX_CHECK(dx.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&dx.fence)))
		dx.fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	}

	//
	// Create descriptor heaps.
	//
	{
		// RTV heap.
		{
			D3D12_DESCRIPTOR_HEAP_DESC heap_desc;
			heap_desc.NumDescriptors = SWAPCHAIN_BUFFER_COUNT;
			heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
			heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
			heap_desc.NodeMask = 0;
			DX_CHECK(dx.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&dx.rtv_heap)))
			dx.rtv_descriptor_size = dx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		}

		// DSV heap.
		{
			D3D12_DESCRIPTOR_HEAP_DESC heap_desc;
			heap_desc.NumDescriptors = 1;
			heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
			heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
			heap_desc.NodeMask = 0;
			DX_CHECK(dx.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&dx.dsv_heap)))
		}

		// SRV heap.
		{
			D3D12_DESCRIPTOR_HEAP_DESC heap_desc;
			heap_desc.NumDescriptors = MAX_DRAWIMAGES;
			heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			heap_desc.NodeMask = 0;
			DX_CHECK(dx.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&dx.srv_heap)))
			dx.srv_descriptor_size = dx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		}

		// Sampler heap.
		{
			D3D12_DESCRIPTOR_HEAP_DESC heap_desc;
			heap_desc.NumDescriptors = SAMPLER_COUNT;
			heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
			heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			heap_desc.NodeMask = 0;
			DX_CHECK(dx.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&dx.sampler_heap)))
			dx.sampler_descriptor_size = dx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
		}
	}

	//
	// Create descriptors.
	//
	{
		// RTV descriptors.
		{
			D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = dx.rtv_heap->GetCPUDescriptorHandleForHeapStart();
			for (int i = 0; i < SWAPCHAIN_BUFFER_COUNT; i++) {
				dx.device->CreateRenderTargetView(dx.render_targets[i], nullptr, rtv_handle);
				rtv_handle.ptr += dx.rtv_descriptor_size;
			}
		}

		// Samplers.
		{
			{
				Vk_Sampler_Def def;
				def.repeat_texture = true;
				def.gl_mag_filter = gl_filter_max;
				def.gl_min_filter = gl_filter_min;
				dx_create_sampler_descriptor(def, SAMPLER_MIP_REPEAT);
			}
			{
				Vk_Sampler_Def def;
				def.repeat_texture = false;
				def.gl_mag_filter = gl_filter_max;
				def.gl_min_filter = gl_filter_min;
				dx_create_sampler_descriptor(def, SAMPLER_MIP_CLAMP);
			}
			{
				Vk_Sampler_Def def;
				def.repeat_texture = true;
				def.gl_mag_filter = GL_LINEAR;
				def.gl_min_filter = GL_LINEAR;
				dx_create_sampler_descriptor(def, SAMPLER_NOMIP_REPEAT);
			}
			{
				Vk_Sampler_Def def;
				def.repeat_texture = false;
				def.gl_mag_filter = GL_LINEAR;
				def.gl_min_filter = GL_LINEAR;
				dx_create_sampler_descriptor(def, SAMPLER_NOMIP_CLAMP);
			}
		}
	}

	//
	// Create depth buffer resources.
	//
	{
		D3D12_RESOURCE_DESC depth_desc{};
		depth_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		depth_desc.Alignment = 0;
		depth_desc.Width = glConfig.vidWidth;
		depth_desc.Height = glConfig.vidHeight;
		depth_desc.DepthOrArraySize = 1;
		depth_desc.MipLevels = 1;
		depth_desc.Format = get_depth_format();
		depth_desc.SampleDesc.Count = 1;
		depth_desc.SampleDesc.Quality = 0;
		depth_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

		D3D12_CLEAR_VALUE optimized_clear_value{};
		optimized_clear_value.Format = get_depth_format();
		optimized_clear_value.DepthStencil.Depth = 1.0f;
		optimized_clear_value.DepthStencil.Stencil = 0;

		D3D12_HEAP_PROPERTIES heap_properties = get_heap_properties(D3D12_HEAP_TYPE_DEFAULT);

		DX_CHECK(dx.device->CreateCommittedResource(
			&heap_properties,
			D3D12_HEAP_FLAG_NONE,
			&depth_desc,
			D3D12_RESOURCE_STATE_DEPTH_WRITE,
			&optimized_clear_value,
			IID_PPV_ARGS(&dx.depth_stencil_buffer)))

		D3D12_DEPTH_STENCIL_VIEW_DESC view_desc{};
		view_desc.Format = get_depth_format();
		view_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
		view_desc.Flags = D3D12_DSV_FLAG_NONE;

		dx.device->CreateDepthStencilView(dx.depth_stencil_buffer, &view_desc,
			dx.dsv_heap->GetCPUDescriptorHandleForHeapStart());
	}

	//
	// Create root signature.
	//
	{
		D3D12_DESCRIPTOR_RANGE ranges[4] = {};
		ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		ranges[0].NumDescriptors = 1;
		ranges[0].BaseShaderRegister = 0;

		ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
		ranges[1].NumDescriptors = 1;
		ranges[1].BaseShaderRegister = 0;

		ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		ranges[2].NumDescriptors = 1;
		ranges[2].BaseShaderRegister = 1;

		ranges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
		ranges[3].NumDescriptors = 1;
		ranges[3].BaseShaderRegister = 1;

		D3D12_ROOT_PARAMETER root_parameters[5] {};

		root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		root_parameters[0].Constants.ShaderRegister = 0;
		root_parameters[0].Constants.Num32BitValues = 32;
		root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		for (int i = 1; i < 5; i++) {
			root_parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			root_parameters[i].DescriptorTable.NumDescriptorRanges = 1;
			root_parameters[i].DescriptorTable.pDescriptorRanges = &ranges[i-1];
			root_parameters[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		}

		D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
		root_signature_desc.NumParameters = _countof(root_parameters);
		root_signature_desc.pParameters = root_parameters;
		root_signature_desc.NumStaticSamplers = 0;
		root_signature_desc.pStaticSamplers = nullptr;
		root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

		ID3DBlob* signature;
		ID3DBlob* error;
		DX_CHECK(D3D12SerializeRootSignature(&root_signature_desc, D3D_ROOT_SIGNATURE_VERSION_1,
			&signature, &error))
		DX_CHECK(dx.device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
			IID_PPV_ARGS(&dx.root_signature)))

		if (signature != nullptr)
			signature->Release();
		if (error != nullptr)
			error->Release();
	}

	//
	// Geometry buffers.
	//
	{
		D3D12_HEAP_PROPERTIES heap_properties = get_heap_properties(D3D12_HEAP_TYPE_UPLOAD);
		D3D12_RESOURCE_DESC buffer_desc = get_buffer_desc(VERTEX_BUFFER_SIZE + INDEX_BUFFER_SIZE);
		// store geometry in upload heap since Q3 regenerates it every frame
		DX_CHECK(dx.device->CreateCommittedResource(
			&heap_properties,
			D3D12_HEAP_FLAG_NONE,
			&buffer_desc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&dx.geometry_buffer)))

		void* p_data;
		D3D12_RANGE read_range{};
        DX_CHECK(dx.geometry_buffer->Map(0, &read_range, &p_data))

		dx.vertex_buffer_ptr = static_cast<byte*>(p_data);

		assert((VERTEX_BUFFER_SIZE & 0xffff) == 0); // index buffer offset should be 64K aligned.
		dx.index_buffer_ptr = static_cast<byte*>(p_data) + VERTEX_BUFFER_SIZE;
	}

	//
	// Standard pipelines.
	//
	{
		// skybox
		{
			Vk_Pipeline_Def def;
			def.shader_type = Vk_Shader_Type::single_texture;
			def.state_bits = 0;
			def.face_culling = CT_FRONT_SIDED;
			def.polygon_offset = false;
			def.clipping_plane = false;
			def.mirror = false;
			dx.skybox_pipeline = create_pipeline(def);
		}

		// Q3 stencil shadows
		{
			{
				Vk_Pipeline_Def def;
				def.polygon_offset = false;
				def.state_bits = 0;
				def.shader_type = Vk_Shader_Type::single_texture;
				def.clipping_plane = false;
				def.shadow_phase = Vk_Shadow_Phase::shadow_edges_rendering;

				cullType_t cull_types[2] = {CT_FRONT_SIDED, CT_BACK_SIDED};
				bool mirror_flags[2] = {false, true};

				for (int i = 0; i < 2; i++) {
					def.face_culling = cull_types[i];
					for (int j = 0; j < 2; j++) {
						def.mirror = mirror_flags[j];
						dx.shadow_volume_pipelines[i][j] = create_pipeline(def);
					}
				}
			}

			{
				Vk_Pipeline_Def def;
				def.face_culling = CT_FRONT_SIDED;
				def.polygon_offset = false;
				def.state_bits = GLS_DEPTHMASK_TRUE | GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO;
				def.shader_type = Vk_Shader_Type::single_texture;
				def.clipping_plane = false;
				def.mirror = false;
				def.shadow_phase = Vk_Shadow_Phase::fullscreen_quad_rendering;
				dx.shadow_finish_pipeline = create_pipeline(def);
			}
		}

		// fog and dlights
		{
			Vk_Pipeline_Def def;
			def.shader_type = Vk_Shader_Type::single_texture;
			def.clipping_plane = false;
			def.mirror = false;

			unsigned int fog_state_bits[2] = {
				GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHFUNC_EQUAL,
				GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA
			};
			unsigned int dlight_state_bits[2] = {
				GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_EQUAL,
				GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_EQUAL
			};
			bool polygon_offset[2] = {false, true};

			for (int i = 0; i < 2; i++) {
				unsigned fog_state = fog_state_bits[i];
				unsigned dlight_state = dlight_state_bits[i];

				for (int j = 0; j < 3; j++) {
					def.face_culling = j; // cullType_t value

					for (int k = 0; k < 2; k++) {
						def.polygon_offset = polygon_offset[k];

						def.state_bits = fog_state;
						dx.fog_pipelines[i][j][k] = create_pipeline(def);

						def.state_bits = dlight_state;
						dx.dlight_pipelines[i][j][k] = create_pipeline(def);
					}
				}
			}
		}

		// debug pipelines
		{
			Vk_Pipeline_Def def;
			def.state_bits = GLS_POLYMODE_LINE | GLS_DEPTHMASK_TRUE;
			dx.tris_debug_pipeline = create_pipeline(def);
		}
		{
			Vk_Pipeline_Def def;
			def.state_bits = GLS_POLYMODE_LINE | GLS_DEPTHMASK_TRUE;
			def.face_culling = CT_BACK_SIDED;
			dx.tris_mirror_debug_pipeline = create_pipeline(def);
		}
		{
			Vk_Pipeline_Def def;
			def.state_bits = GLS_DEPTHMASK_TRUE;
			def.line_primitives = true;
			dx.normals_debug_pipeline = create_pipeline(def);
		}
		{
			Vk_Pipeline_Def def;
			def.state_bits = GLS_DEPTHMASK_TRUE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE;
			dx.surface_debug_pipeline_solid = create_pipeline(def);
		}
		{
			Vk_Pipeline_Def def;
			def.state_bits = GLS_POLYMODE_LINE | GLS_DEPTHMASK_TRUE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE;
			def.line_primitives = true;
			dx.surface_debug_pipeline_outline = create_pipeline(def);
		}
		{
			Vk_Pipeline_Def def;
			def.state_bits = GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA;
			dx.images_debug_pipeline = create_pipeline(def);
		}
	}

	dx.active = true;
}

void dx_shutdown() {
	::CloseHandle(dx.fence_event);

	for (int i = 0; i < SWAPCHAIN_BUFFER_COUNT; i++) {
		dx.render_targets[i]->Release();
	}
	for (int i = 0; i < 2; i++) {
		for (int j = 0; j < 2; j++) {
			dx.shadow_volume_pipelines[i][j]->Release();
		}
	}
	for (int i = 0; i < 2; i++) {
		for (int j = 0; j < 3; j++) {
			for (int k = 0; k < 2; k++) {
				dx.fog_pipelines[i][j][k]->Release();
				dx.dlight_pipelines[i][j][k]->Release();
			}
		}
	}

	dx.swapchain->Release();
	dx.command_allocator->Release();
	dx.helper_command_allocator->Release();
	dx.rtv_heap->Release();
	dx.srv_heap->Release();
	dx.sampler_heap->Release();
	dx.root_signature->Release();
	dx.command_queue->Release();
	dx.command_list->Release();
	dx.fence->Release();
	dx.depth_stencil_buffer->Release();
	dx.dsv_heap->Release();
	dx.geometry_buffer->Release();
	dx.skybox_pipeline->Release();
	dx.shadow_finish_pipeline->Release();
	dx.tris_debug_pipeline->Release();
	dx.tris_mirror_debug_pipeline->Release();
	dx.normals_debug_pipeline->Release();
	dx.surface_debug_pipeline_solid->Release();
	dx.surface_debug_pipeline_outline->Release();
	dx.images_debug_pipeline->Release();

	dx.device->Release();

	Com_Memset(&dx, 0, sizeof(dx));
}

void dx_release_resources() {
	dx_wait_device_idle();

	dx_world.pipeline_create_time = 0.0f;
	for (int i = 0; i < dx_world.num_pipelines; i++) {
		dx_world.pipelines[i]->Release();
	}

	for (int i = 0; i < MAX_VK_IMAGES; i++) {
		if (dx_world.images[i].texture != nullptr) {
			dx_world.images[i].texture->Release();
		}
	}

	Com_Memset(&dx_world, 0, sizeof(dx_world));

	// Reset geometry buffer's current offsets.
	dx.xyz_elements = 0;
	dx.color_st_elements = 0;
	dx.index_buffer_offset = 0;
}

void dx_wait_device_idle() {
	dx.fence_value++;
	DX_CHECK(dx.command_queue->Signal(dx.fence, dx.fence_value))
	DX_CHECK(dx.fence->SetEventOnCompletion(dx.fence_value, dx.fence_event))
	WaitForSingleObject(dx.fence_event, INFINITE);
}

Dx_Image dx_create_image(int width, int height, Dx_Image_Format format, int mip_levels,  bool repeat_texture, int image_index) {
	Dx_Image image;

	DXGI_FORMAT dx_format;
	if (format == IMAGE_FORMAT_RGBA8)
		dx_format = DXGI_FORMAT_R8G8B8A8_UNORM;
	else if (format == IMAGE_FORMAT_BGRA4)
		dx_format = DXGI_FORMAT_B4G4R4A4_UNORM;
	else {
		assert(format == IMAGE_FORMAT_BGR5A1);
		dx_format = DXGI_FORMAT_B5G5R5A1_UNORM;
	}

	// create texture
	{
		D3D12_RESOURCE_DESC desc;
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Alignment = 0;
		desc.Width = width;
		desc.Height = height;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = mip_levels;
		desc.Format = dx_format;
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = 0;
		desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		desc.Flags = D3D12_RESOURCE_FLAG_NONE;
		D3D12_HEAP_PROPERTIES heap_properties = get_heap_properties(D3D12_HEAP_TYPE_DEFAULT);
		DX_CHECK(dx.device->CreateCommittedResource(
			&heap_properties,
			D3D12_HEAP_FLAG_NONE,
			&desc,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
			nullptr,
			IID_PPV_ARGS(&image.texture)))
	}

	// create texture descriptor
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
		srv_desc.Format = dx_format;
		srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv_desc.Texture2D.MipLevels = mip_levels;

		D3D12_CPU_DESCRIPTOR_HANDLE handle;
		handle.ptr = dx.srv_heap->GetCPUDescriptorHandleForHeapStart().ptr + image_index * dx.srv_descriptor_size;
		dx.device->CreateShaderResourceView(image.texture, &srv_desc, handle);

		dx_world.current_image_indices[glState.currenttmu] = image_index;
	}

	if (mip_levels > 0)
		image.sampler_index = repeat_texture ? SAMPLER_MIP_REPEAT : SAMPLER_MIP_CLAMP;
	else
		image.sampler_index = repeat_texture ? SAMPLER_NOMIP_REPEAT : SAMPLER_NOMIP_CLAMP;

	return image;
}

void dx_upload_image_data(ID3D12Resource* texture, int width, int height, int mip_levels, const uint8_t* pixels, int bytes_per_pixel) {
	//
	// Initialize subresource layouts int the upload texture.
	//
	auto align =[](size_t value, size_t alignment) {
		return (value + alignment - 1) & ~(alignment - 1);
	};

	D3D12_PLACED_SUBRESOURCE_FOOTPRINT regions[16];
	UINT64 buffer_size = 0;

	int w = width;
	int h = height;
	for (int i = 0; i < mip_levels; i++) {
		regions[i].Offset = buffer_size;
		regions[i].Footprint.Format = texture->GetDesc().Format;
		regions[i].Footprint.Width = w;
		regions[i].Footprint.Height = h;
		regions[i].Footprint.Depth = 1;
		regions[i].Footprint.RowPitch = static_cast<UINT>(align(w * bytes_per_pixel, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
		buffer_size += align(regions[i].Footprint.RowPitch * h, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
		w >>= 1;
		if (w < 1) w = 1;
		h >>= 1;
		if (h < 1) h = 1;
	}

	//
	// Create upload upload texture.
	//
	ID3D12Resource* upload_texture;
	D3D12_HEAP_PROPERTIES heap_properties = get_heap_properties(D3D12_HEAP_TYPE_UPLOAD);
	D3D12_RESOURCE_DESC buffer_desc = get_buffer_desc(buffer_size);
	DX_CHECK(dx.device->CreateCommittedResource(
			&heap_properties,
			D3D12_HEAP_FLAG_NONE,
			&buffer_desc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&upload_texture)))

	byte* upload_texture_data;
	DX_CHECK(upload_texture->Map(0, nullptr, reinterpret_cast<void**>(&upload_texture_data)))
	w = width;
	h = height;
	for (int i = 0; i < mip_levels; i++) {
		byte* upload_subresource_base = upload_texture_data + regions[i].Offset;
		for (int y = 0; y < h; y++) {
			Com_Memcpy(upload_subresource_base + regions[i].Footprint.RowPitch * y, pixels, w * bytes_per_pixel);
			pixels += w * bytes_per_pixel;
		}
		w >>= 1;
		if (w < 1) w = 1;
		h >>= 1;
		if (h < 1) h = 1;
	}
	upload_texture->Unmap(0, nullptr);

	//
	// Copy data from upload texture to destination texture.
	//
	record_and_run_commands([texture, upload_texture, &regions, mip_levels]
		(ID3D12GraphicsCommandList* command_list)
	{

		D3D12_RESOURCE_BARRIER transition_barrier = get_transition_barrier(texture,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
		command_list->ResourceBarrier(1, &transition_barrier);

		for (UINT i = 0; i < mip_levels; ++i) {
			D3D12_TEXTURE_COPY_LOCATION  dst;
			dst.pResource = texture;
			dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			dst.SubresourceIndex = i;

			D3D12_TEXTURE_COPY_LOCATION src;
			src.pResource = upload_texture;
			src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			src.PlacedFootprint = regions[i];

			command_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
		}

		transition_barrier = get_transition_barrier(texture,
													D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		command_list->ResourceBarrier(1, &transition_barrier);
	});

	upload_texture->Release();
}

static ID3D12PipelineState* create_pipeline(const Vk_Pipeline_Def& def) {
	// single texture VS
	extern unsigned char single_texture_vs[];
	extern long long single_texture_vs_size;

	extern unsigned char single_texture_clipping_plane_vs[];
	extern long long single_texture_clipping_plane_vs_size;

	// multi texture VS
	extern unsigned char multi_texture_vs[];
	extern long long multi_texture_vs_size;

	extern unsigned char multi_texture_clipping_plane_vs[];
	extern long long multi_texture_clipping_plane_vs_size;

	// single texture PS
	extern unsigned char single_texture_ps[];
	extern long long single_texture_ps_size;

	extern unsigned char single_texture_gt0_ps[];
	extern long long single_texture_gt0_ps_size;

	extern unsigned char single_texture_lt80_ps[];
	extern long long single_texture_lt80_ps_size;

	extern unsigned char single_texture_ge80_ps[];
	extern long long single_texture_ge80_ps_size;

	// multi texture mul PS
	extern unsigned char multi_texture_mul_ps[];
	extern long long multi_texture_mul_ps_size;

	extern unsigned char multi_texture_mul_gt0_ps[];
	extern long long multi_texture_mul_gt0_ps_size;

	extern unsigned char multi_texture_mul_lt80_ps[];
	extern long long multi_texture_mul_lt80_ps_size;

	extern unsigned char multi_texture_mul_ge80_ps[];
	extern long long multi_texture_mul_ge80_ps_size;

	// multi texture add PS
	extern unsigned char multi_texture_add_ps[];
	extern long long multi_texture_add_ps_size;

	extern unsigned char multi_texture_add_gt0_ps[];
	extern long long multi_texture_add_gt0_ps_size;

	extern unsigned char multi_texture_add_lt80_ps[];
	extern long long multi_texture_add_lt80_ps_size;

	extern unsigned char multi_texture_add_ge80_ps[];
	extern long long multi_texture_add_ge80_ps_size;

#define BYTECODE(name) D3D12_SHADER_BYTECODE{name, (SIZE_T)name##_size}

#define GET_PS_BYTECODE(base_name) \
	if ((def.state_bits & GLS_ATEST_BITS) == 0) \
		ps_bytecode = BYTECODE(base_name##_ps); \
	else if (def.state_bits & GLS_ATEST_GT_0) \
		ps_bytecode = BYTECODE(base_name##_gt0_ps); \
	else if (def.state_bits & GLS_ATEST_LT_80) \
		ps_bytecode = BYTECODE(base_name##_lt80_ps); \
	else if (def.state_bits & GLS_ATEST_GE_80) \
		ps_bytecode = BYTECODE(base_name##_ge80_ps); \
	else \
		ri.Error(ERR_DROP, "create_pipeline: invalid alpha test state bits\n");

	D3D12_SHADER_BYTECODE vs_bytecode;
	D3D12_SHADER_BYTECODE ps_bytecode;
	if (def.shader_type == Vk_Shader_Type::single_texture) {
		if (def.clipping_plane) {
			vs_bytecode = BYTECODE(single_texture_clipping_plane_vs);
		} else {
			vs_bytecode = BYTECODE(single_texture_vs);
		}
		GET_PS_BYTECODE(single_texture)
	} else if (def.shader_type == Vk_Shader_Type::multi_texture_mul) {
		if (def.clipping_plane) {
			vs_bytecode = BYTECODE(multi_texture_clipping_plane_vs);
		} else {
			vs_bytecode = BYTECODE(multi_texture_vs);
		}
		GET_PS_BYTECODE(multi_texture_mul)
	} else if (def.shader_type == Vk_Shader_Type::multi_texture_add) {
		if (def.clipping_plane) {
			vs_bytecode = BYTECODE(multi_texture_clipping_plane_vs);
		} else {
			vs_bytecode = BYTECODE(multi_texture_vs);
		}
		GET_PS_BYTECODE(multi_texture_add)
	}

#undef GET_PS_BYTECODE
#undef BYTECODE

	// Vertex elements.
	D3D12_INPUT_ELEMENT_DESC input_element_desc[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 2, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 3, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};

	//
	// Blend.
	//
	D3D12_BLEND_DESC blend_state;
	blend_state.AlphaToCoverageEnable = FALSE;
	blend_state.IndependentBlendEnable = FALSE;
	auto& rt_blend_desc = blend_state.RenderTarget[0];
	rt_blend_desc.BlendEnable = (def.state_bits & (GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS)) ? TRUE : FALSE;
	rt_blend_desc.LogicOpEnable = FALSE;

	if (rt_blend_desc.BlendEnable) {
		switch (def.state_bits & GLS_SRCBLEND_BITS) {
			case GLS_SRCBLEND_ZERO:
				rt_blend_desc.SrcBlend = D3D12_BLEND_ZERO;
				rt_blend_desc.SrcBlendAlpha = D3D12_BLEND_ZERO;
				break;
			case GLS_SRCBLEND_ONE:
				rt_blend_desc.SrcBlend = D3D12_BLEND_ONE;
				rt_blend_desc.SrcBlendAlpha = D3D12_BLEND_ONE;
				break;
			case GLS_SRCBLEND_DST_COLOR:
				rt_blend_desc.SrcBlend = D3D12_BLEND_DEST_COLOR;
				rt_blend_desc.SrcBlendAlpha = D3D12_BLEND_DEST_ALPHA;
				break;
			case GLS_SRCBLEND_ONE_MINUS_DST_COLOR:
				rt_blend_desc.SrcBlend = D3D12_BLEND_INV_DEST_COLOR;
				rt_blend_desc.SrcBlendAlpha = D3D12_BLEND_INV_DEST_ALPHA;
				break;
			case GLS_SRCBLEND_SRC_ALPHA:
				rt_blend_desc.SrcBlend = D3D12_BLEND_SRC_ALPHA;
				rt_blend_desc.SrcBlendAlpha = D3D12_BLEND_SRC_ALPHA;
				break;
			case GLS_SRCBLEND_ONE_MINUS_SRC_ALPHA:
				rt_blend_desc.SrcBlend = D3D12_BLEND_INV_SRC_ALPHA;
				rt_blend_desc.SrcBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
				break;
			case GLS_SRCBLEND_DST_ALPHA:
				rt_blend_desc.SrcBlend = D3D12_BLEND_DEST_ALPHA;
				rt_blend_desc.SrcBlendAlpha = D3D12_BLEND_DEST_ALPHA;
				break;
			case GLS_SRCBLEND_ONE_MINUS_DST_ALPHA:
				rt_blend_desc.SrcBlend = D3D12_BLEND_INV_DEST_ALPHA;
				rt_blend_desc.SrcBlendAlpha = D3D12_BLEND_INV_DEST_ALPHA;
				break;
			case GLS_SRCBLEND_ALPHA_SATURATE:
				rt_blend_desc.SrcBlend = D3D12_BLEND_SRC_ALPHA_SAT;
				rt_blend_desc.SrcBlendAlpha = D3D12_BLEND_SRC_ALPHA_SAT;
				break;
			default:
				ri.Error( ERR_DROP, "create_pipeline: invalid src blend state bits\n" );
				break;
		}
		switch (def.state_bits & GLS_DSTBLEND_BITS) {
			case GLS_DSTBLEND_ZERO:
				rt_blend_desc.DestBlend = D3D12_BLEND_ZERO;
				rt_blend_desc.DestBlendAlpha = D3D12_BLEND_ZERO;
				break;
			case GLS_DSTBLEND_ONE:
				rt_blend_desc.DestBlend = D3D12_BLEND_ONE;
				rt_blend_desc.DestBlendAlpha = D3D12_BLEND_ONE;
				break;
			case GLS_DSTBLEND_SRC_COLOR:
				rt_blend_desc.DestBlend = D3D12_BLEND_SRC_COLOR;
				rt_blend_desc.DestBlendAlpha = D3D12_BLEND_SRC_ALPHA;
				break;
			case GLS_DSTBLEND_ONE_MINUS_SRC_COLOR:
				rt_blend_desc.DestBlend = D3D12_BLEND_INV_SRC_COLOR;
				rt_blend_desc.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
				break;
			case GLS_DSTBLEND_SRC_ALPHA:
				rt_blend_desc.DestBlend = D3D12_BLEND_SRC_ALPHA;
				rt_blend_desc.DestBlendAlpha = D3D12_BLEND_SRC_ALPHA;
				break;
			case GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA:
				rt_blend_desc.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
				rt_blend_desc.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
				break;
			case GLS_DSTBLEND_DST_ALPHA:
				rt_blend_desc.DestBlend = D3D12_BLEND_DEST_ALPHA;
				rt_blend_desc.DestBlendAlpha = D3D12_BLEND_DEST_ALPHA;
				break;
			case GLS_DSTBLEND_ONE_MINUS_DST_ALPHA:
				rt_blend_desc.DestBlend = D3D12_BLEND_INV_DEST_ALPHA;
				rt_blend_desc.DestBlendAlpha = D3D12_BLEND_INV_DEST_ALPHA;
				break;
			default:
				ri.Error( ERR_DROP, "create_pipeline: invalid dst blend state bits\n" );
				break;
		}
	}
	rt_blend_desc.BlendOp = D3D12_BLEND_OP_ADD;
	rt_blend_desc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
	rt_blend_desc.LogicOp = D3D12_LOGIC_OP_COPY;
	rt_blend_desc.RenderTargetWriteMask = (def.shadow_phase == Vk_Shadow_Phase::shadow_edges_rendering) ? 0 : D3D12_COLOR_WRITE_ENABLE_ALL;

	//
	// Rasteriazation state.
	//
	D3D12_RASTERIZER_DESC rasterization_state = {};
	rasterization_state.FillMode = (def.state_bits & GLS_POLYMODE_LINE) ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;

	if (def.face_culling == CT_TWO_SIDED)
		rasterization_state.CullMode = D3D12_CULL_MODE_NONE;
	else if (def.face_culling == CT_FRONT_SIDED)
		rasterization_state.CullMode = (def.mirror ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_BACK);
	else if (def.face_culling == CT_BACK_SIDED)
		rasterization_state.CullMode = (def.mirror ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_FRONT);
	else
		ri.Error(ERR_DROP, "create_pipeline: invalid face culling mode\n");

	rasterization_state.FrontCounterClockwise = FALSE; // Q3 defaults to clockwise vertex order
	rasterization_state.DepthBias = def.polygon_offset ? r_offsetUnits->integer : 0;
	rasterization_state.DepthBiasClamp = 0.0f;
	rasterization_state.SlopeScaledDepthBias = def.polygon_offset ? r_offsetFactor->value : 0.0f;
	rasterization_state.DepthClipEnable = TRUE;
	rasterization_state.MultisampleEnable = FALSE;
	rasterization_state.AntialiasedLineEnable = FALSE;
	rasterization_state.ForcedSampleCount = 0;
	rasterization_state.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

	//
	// Depth/stencil state.
	//
	D3D12_DEPTH_STENCIL_DESC depth_stencil_state = {};
	depth_stencil_state.DepthEnable = (def.state_bits & GLS_DEPTHTEST_DISABLE) ? FALSE : TRUE;
	depth_stencil_state.DepthWriteMask = (def.state_bits & GLS_DEPTHMASK_TRUE) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
	depth_stencil_state.DepthFunc = (def.state_bits & GLS_DEPTHFUNC_EQUAL) ? D3D12_COMPARISON_FUNC_EQUAL : D3D12_COMPARISON_FUNC_LESS_EQUAL;
	depth_stencil_state.StencilEnable = (def.shadow_phase != Vk_Shadow_Phase::disabled) ? TRUE : FALSE;
	depth_stencil_state.StencilReadMask = 255;
	depth_stencil_state.StencilWriteMask = 255;

	if (def.shadow_phase == Vk_Shadow_Phase::shadow_edges_rendering) {
		depth_stencil_state.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		depth_stencil_state.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		depth_stencil_state.FrontFace.StencilPassOp = (def.face_culling == CT_FRONT_SIDED) ? D3D12_STENCIL_OP_INCR_SAT : D3D12_STENCIL_OP_DECR_SAT;
		depth_stencil_state.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;

		depth_stencil_state.BackFace = depth_stencil_state.FrontFace;
	} else if (def.shadow_phase == Vk_Shadow_Phase::fullscreen_quad_rendering) {
		depth_stencil_state.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		depth_stencil_state.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		depth_stencil_state.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
		depth_stencil_state.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_NOT_EQUAL;

		depth_stencil_state.BackFace = depth_stencil_state.FrontFace;
	} else {
		depth_stencil_state.FrontFace = {};
		depth_stencil_state.BackFace = {};
	}

	//
	// Create pipeline state.
	//
	D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline_desc = {};
	pipeline_desc.pRootSignature = dx.root_signature;
	pipeline_desc.VS = vs_bytecode;
	pipeline_desc.PS = ps_bytecode;
	pipeline_desc.BlendState = blend_state;
	pipeline_desc.SampleMask = UINT_MAX;
	pipeline_desc.RasterizerState = rasterization_state;
	pipeline_desc.DepthStencilState = depth_stencil_state;
	pipeline_desc.InputLayout = { input_element_desc, def.shader_type == Vk_Shader_Type::single_texture ? 3u : 4u };
	pipeline_desc.PrimitiveTopologyType = def.line_primitives ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pipeline_desc.NumRenderTargets = 1;
	pipeline_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	pipeline_desc.DSVFormat = get_depth_format();
	pipeline_desc.SampleDesc.Count = 1;
	pipeline_desc.SampleDesc.Quality = 0;

	ID3D12PipelineState* pipeline;
	DX_CHECK(dx.device->CreateGraphicsPipelineState(&pipeline_desc, IID_PPV_ARGS(&pipeline)))
	return pipeline;
}

struct Timer {
	using Clock = std::chrono::high_resolution_clock;
	using Second = std::chrono::duration<double, std::ratio<1>>;

	Clock::time_point start = Clock::now();
	double elapsed_seconds() const {
		const auto duration = Clock::now() - start;
		double seconds = std::chrono::duration_cast<Second>(duration).count();
		return seconds;
	}
};

void dx_create_sampler_descriptor(const Vk_Sampler_Def& def, Dx_Sampler_Index sampler_index)
{
	uint32_t min, mag, mip;

	if (def.gl_mag_filter == GL_NEAREST) {
		mag = 0;
	} else if (def.gl_mag_filter == GL_LINEAR) {
		mag = 1;
	} else {
		ri.Error(ERR_FATAL, "create_sampler_descriptor: invalid gl_mag_filter");
	}

	bool max_lod_0_25 = false; // used to emulate OpenGL's GL_LINEAR/GL_NEAREST minification filter
	if (def.gl_min_filter == GL_NEAREST) {
		min = 0;
		mip = 0;
		max_lod_0_25 = true;
	} else if (def.gl_min_filter == GL_LINEAR) {
		min = 1;
		mip = 0;
		max_lod_0_25 = true;
	} else if (def.gl_min_filter == GL_NEAREST_MIPMAP_NEAREST) {
		min = 0;
		mip = 0;
	} else if (def.gl_min_filter == GL_LINEAR_MIPMAP_NEAREST) {
		min = 1;
		mip = 0;
	} else if (def.gl_min_filter == GL_NEAREST_MIPMAP_LINEAR) {
		min = 0;
		mip = 1;
	} else if (def.gl_min_filter == GL_LINEAR_MIPMAP_LINEAR) {
		min = 1;
		mip = 1;
	} else {
		ri.Error(ERR_FATAL, "vk_find_sampler: invalid gl_min_filter");
	}

	D3D12_TEXTURE_ADDRESS_MODE address_mode = def.repeat_texture ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

	D3D12_SAMPLER_DESC sampler_desc;
	sampler_desc.Filter = D3D12_ENCODE_BASIC_FILTER(min, mag, mip, 0);
	sampler_desc.AddressU = address_mode;
	sampler_desc.AddressV = address_mode;
	sampler_desc.AddressW = address_mode;
	sampler_desc.MipLODBias = 0.0f;
	sampler_desc.MaxAnisotropy = 1;
	sampler_desc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	sampler_desc.BorderColor[0] = 0.0f;
	sampler_desc.BorderColor[1] = 0.0f;
	sampler_desc.BorderColor[2] = 0.0f;
	sampler_desc.BorderColor[3] = 0.0f;
	sampler_desc.MinLOD = 0.0f;
	sampler_desc.MaxLOD = max_lod_0_25 ? 0.25f : 12.0f;

	D3D12_CPU_DESCRIPTOR_HANDLE sampler_handle = dx.sampler_heap->GetCPUDescriptorHandleForHeapStart();
	sampler_handle.ptr += dx.sampler_descriptor_size * sampler_index;

	dx.device->CreateSampler(&sampler_desc, sampler_handle);
}

ID3D12PipelineState* dx_find_pipeline(const Vk_Pipeline_Def& def) {
	for (int i = 0; i < dx_world.num_pipelines; i++) {
		const auto& cur_def = dx_world.pipeline_defs[i];

		if (cur_def.shader_type == def.shader_type &&
			cur_def.state_bits == def.state_bits &&
			cur_def.face_culling == def.face_culling &&
			cur_def.polygon_offset == def.polygon_offset &&
			cur_def.clipping_plane == def.clipping_plane &&
			cur_def.mirror == def.mirror &&
			cur_def.line_primitives == def.line_primitives &&
			cur_def.shadow_phase == def.shadow_phase)
		{
			return dx_world.pipelines[i];
		}
	}

	if (dx_world.num_pipelines >= MAX_VK_PIPELINES) {
		ri.Error(ERR_DROP, "dx_find_pipeline: MAX_VK_PIPELINES hit\n");
	}

	Timer t;
	ID3D12PipelineState* pipeline = create_pipeline(def);
	dx_world.pipeline_create_time += t.elapsed_seconds();

	dx_world.pipeline_defs[dx_world.num_pipelines] = def;
	dx_world.pipelines[dx_world.num_pipelines] = pipeline;
	dx_world.num_pipelines++;
	return pipeline;
}

static void get_mvp_transform(float* mvp) {
	if (backEnd.projection2D) {
		float mvp0 = 2.0f / glConfig.vidWidth;
		float mvp5 = 2.0f / glConfig.vidHeight;

		mvp[0]  =  mvp0; mvp[1]  =  0.0f; mvp[2]  = 0.0f; mvp[3]  = 0.0f;
		mvp[4]  =  0.0f; mvp[5]  = -mvp5; mvp[6]  = 0.0f; mvp[7]  = 0.0f;
		mvp[8]  =  0.0f; mvp[9]  = 0.0f; mvp[10] = 1.0f; mvp[11] = 0.0f;
		mvp[12] = -1.0f; mvp[13] = 1.0f; mvp[14] = 0.0f; mvp[15] = 1.0f;

	} else {
		const float* p = backEnd.viewParms.projectionMatrix;

		// update q3's proj matrix (opengl) to d3d conventions: z - [0, 1] instead of [-1, 1]
		float zNear	= r_znear->value;
		float zFar = backEnd.viewParms.zFar;
		float P10 = -zFar / (zFar - zNear);
		float P14 = -zFar*zNear / (zFar - zNear);

		float proj[16] = {
			p[0],  p[1],  p[2], p[3],
			p[4],  p[5],  p[6], p[7],
			p[8],  p[9],  P10,  p[11],
			p[12], p[13], P14,  p[15]
		};

		myGlMultMatrix(dx_world.modelview_transform, proj, mvp);
	}
}

static D3D12_RECT get_viewport_rect() {
	D3D12_RECT r;
	if (backEnd.projection2D) {
		r.left = 0.0f;
		r.top = 0.0f;
		r.right = glConfig.vidWidth;
		r.bottom = glConfig.vidHeight;
	} else {
		r.left = backEnd.viewParms.viewportX;
		r.top = glConfig.vidHeight - (backEnd.viewParms.viewportY + backEnd.viewParms.viewportHeight);
		r.right = r.left + backEnd.viewParms.viewportWidth;
		r.bottom = r.top + backEnd.viewParms.viewportHeight;
	}
	return r;
}

static D3D12_VIEWPORT get_viewport(Vk_Depth_Range depth_range) {
	D3D12_RECT r = get_viewport_rect();

	D3D12_VIEWPORT viewport;
	viewport.TopLeftX = (float)r.left;
	viewport.TopLeftY = (float)r.top;
	viewport.Width = (float)(r.right - r.left);
	viewport.Height = (float)(r.bottom - r.top);

	if (depth_range == Vk_Depth_Range::force_zero) {
		viewport.MinDepth = 0.0f;
		viewport.MaxDepth = 0.0f;
	} else if (depth_range == Vk_Depth_Range::force_one) {
		viewport.MinDepth = 1.0f;
		viewport.MaxDepth = 1.0f;
	} else if (depth_range == Vk_Depth_Range::weapon) {
		viewport.MinDepth = 0.0f;
		viewport.MaxDepth = 0.3f;
	} else {
		viewport.MinDepth = 0.0f;
		viewport.MaxDepth = 1.0f;
	}
	return viewport;
}

static D3D12_RECT get_scissor_rect() {
	D3D12_RECT r = get_viewport_rect();

	if (r.left < 0)
		r.left = 0;
	if (r.top < 0)
		r.top = 0;

	if (r.right > glConfig.vidWidth)
		r.right = glConfig.vidWidth;
	if (r.bottom > glConfig.vidHeight)
		r.bottom = glConfig.vidHeight;

	return r;
}

void dx_clear_attachments(bool clear_depth_stencil, bool clear_color, vec4_t color) {
	if (!dx.active)
		return;

	if (!clear_depth_stencil && !clear_color)
		return;

	D3D12_RECT clear_rect = get_scissor_rect();

	if (clear_depth_stencil) {
		D3D12_CLEAR_FLAGS flags = D3D12_CLEAR_FLAG_DEPTH;
		if (r_shadows->integer == 2)
			flags |= D3D12_CLEAR_FLAG_STENCIL;

		D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle = dx.dsv_heap->GetCPUDescriptorHandleForHeapStart();
		dx.command_list->ClearDepthStencilView(dsv_handle, flags, 1.0f, 0, 1, &clear_rect);
	}

	if (clear_color) {
		D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = dx.rtv_heap->GetCPUDescriptorHandleForHeapStart();
		rtv_handle.ptr += dx.frame_index * dx.rtv_descriptor_size;
		dx.command_list->ClearRenderTargetView(rtv_handle, color, 1, &clear_rect);
	}
}

void dx_bind_geometry() {
	// xyz stream
	{
		if ((dx.xyz_elements + tess.numVertexes) * sizeof(vec4_t) > XYZ_SIZE)
			ri.Error(ERR_DROP, "dx_bind_geometry: vertex buffer overflow (xyz)\n");

		byte* dst = dx.vertex_buffer_ptr + XYZ_OFFSET + dx.xyz_elements * sizeof(vec4_t);
		Com_Memcpy(dst, tess.xyz, tess.numVertexes * sizeof(vec4_t));

		uint32_t xyz_offset = XYZ_OFFSET + dx.xyz_elements * sizeof(vec4_t);

		D3D12_VERTEX_BUFFER_VIEW xyz_view;
		xyz_view.BufferLocation = dx.geometry_buffer->GetGPUVirtualAddress() + xyz_offset;
		xyz_view.SizeInBytes = static_cast<UINT>(tess.numVertexes * sizeof(vec4_t));
		xyz_view.StrideInBytes = static_cast<UINT>(sizeof(vec4_t));
		dx.command_list->IASetVertexBuffers(0, 1, &xyz_view);

		dx.xyz_elements += tess.numVertexes;
	}

	// indexes stream
	{
		std::size_t indexes_size = tess.numIndexes * sizeof(uint32_t);        

		if (dx.index_buffer_offset + indexes_size > INDEX_BUFFER_SIZE)
			ri.Error(ERR_DROP, "dx_bind_geometry: index buffer overflow\n");

		byte* dst = dx.index_buffer_ptr + dx.index_buffer_offset;
		Com_Memcpy(dst, tess.indexes, indexes_size);

		D3D12_INDEX_BUFFER_VIEW index_view;
		index_view.BufferLocation = dx.geometry_buffer->GetGPUVirtualAddress() + VERTEX_BUFFER_SIZE + dx.index_buffer_offset;
		index_view.SizeInBytes = static_cast<UINT>(indexes_size);
		index_view.Format = DXGI_FORMAT_R32_UINT;
		dx.command_list->IASetIndexBuffer(&index_view);

		dx.index_buffer_offset += static_cast<int>(indexes_size);
	}

	//
	// Specify push constants.
	//
	float root_constants[16 + 12 + 4]; // mvp transform + eye transform + clipping plane in eye space

	get_mvp_transform(root_constants);
	int root_constant_count = 16;

	if (backEnd.viewParms.isPortal) {
		// Eye space transform.
		// NOTE: backEnd.or.modelMatrix incorporates s_flipMatrix, so it should be taken into account 
		// when computing clipping plane too.
		float* eye_xform = root_constants + 16;
		for (int i = 0; i < 12; i++) {
			eye_xform[i] = backEnd.orient.modelMatrix[(i%4)*4 + i/4 ];
		}

		// Clipping plane in eye coordinates.
		float world_plane[4];
		world_plane[0] = backEnd.viewParms.portalPlane.normal[0];
		world_plane[1] = backEnd.viewParms.portalPlane.normal[1];
		world_plane[2] = backEnd.viewParms.portalPlane.normal[2];
		world_plane[3] = backEnd.viewParms.portalPlane.dist;

		float eye_plane[4];
		eye_plane[0] = DotProduct (backEnd.viewParms.orient.axis[0], world_plane);
		eye_plane[1] = DotProduct (backEnd.viewParms.orient.axis[1], world_plane);
		eye_plane[2] = DotProduct (backEnd.viewParms.orient.axis[2], world_plane);
		eye_plane[3] = DotProduct (world_plane, backEnd.viewParms.orient.origin) - world_plane[3];

		// Apply s_flipMatrix to be in the same coordinate system as eye_xfrom.
		root_constants[28] = -eye_plane[1];
		root_constants[29] =  eye_plane[2];
		root_constants[30] = -eye_plane[0];
		root_constants[31] =  eye_plane[3];

		root_constant_count += 16;
	}
	dx.command_list->SetGraphicsRoot32BitConstants(0, root_constant_count, root_constants, 0);
}

void dx_shade_geometry(ID3D12PipelineState* pipeline, bool multitexture, Vk_Depth_Range depth_range, bool indexed, bool lines) {
	// color
	{
		if ((dx.color_st_elements + tess.numVertexes) * sizeof(color4ub_t) > COLOR_SIZE)
			ri.Error(ERR_DROP, "vulkan: vertex buffer overflow (color)\n");

		byte* dst = dx.vertex_buffer_ptr + COLOR_OFFSET + dx.color_st_elements * sizeof(color4ub_t);
		Com_Memcpy(dst, tess.svars.colors, tess.numVertexes * sizeof(color4ub_t));
	}
	// st0
	{
		if ((dx.color_st_elements + tess.numVertexes) * sizeof(vec2_t) > ST0_SIZE)
			ri.Error(ERR_DROP, "vulkan: vertex buffer overflow (st0)\n");

		byte* dst = dx.vertex_buffer_ptr + ST0_OFFSET + dx.color_st_elements * sizeof(vec2_t);
		Com_Memcpy(dst, tess.svars.texcoords[0], tess.numVertexes * sizeof(vec2_t));
	}
	// st1
	if (multitexture) {
		if ((dx.color_st_elements + tess.numVertexes) * sizeof(vec2_t) > ST1_SIZE)
			ri.Error(ERR_DROP, "vulkan: vertex buffer overflow (st1)\n");

		byte* dst = dx.vertex_buffer_ptr + ST1_OFFSET + dx.color_st_elements * sizeof(vec2_t);
		Com_Memcpy(dst, tess.svars.texcoords[1], tess.numVertexes * sizeof(vec2_t));
	}

	//
	// Configure vertex data stream.
	//
	D3D12_VERTEX_BUFFER_VIEW color_st_views[3];
	color_st_views[0].BufferLocation = dx.geometry_buffer->GetGPUVirtualAddress() + COLOR_OFFSET + dx.color_st_elements * sizeof(color4ub_t);
	color_st_views[0].SizeInBytes = static_cast<UINT>(tess.numVertexes * sizeof(color4ub_t));
	color_st_views[0].StrideInBytes = static_cast<UINT>(sizeof(color4ub_t));

	color_st_views[1].BufferLocation = dx.geometry_buffer->GetGPUVirtualAddress() + ST0_OFFSET + dx.color_st_elements * sizeof(vec2_t);
	color_st_views[1].SizeInBytes = static_cast<UINT>(tess.numVertexes * sizeof(vec2_t));
	color_st_views[1].StrideInBytes = static_cast<UINT>(sizeof(vec2_t));

	color_st_views[2].BufferLocation = dx.geometry_buffer->GetGPUVirtualAddress() + ST1_OFFSET + dx.color_st_elements * sizeof(vec2_t);
	color_st_views[2].SizeInBytes = static_cast<UINT>(tess.numVertexes * sizeof(vec2_t));
	color_st_views[2].StrideInBytes = static_cast<UINT>(sizeof(vec2_t));

	dx.command_list->IASetVertexBuffers(1, multitexture ? 3 : 2, color_st_views);
	dx.color_st_elements += tess.numVertexes;

	//
	// Set descriptor tables.
	//
	{
		D3D12_GPU_DESCRIPTOR_HANDLE srv_handle = dx.srv_heap->GetGPUDescriptorHandleForHeapStart();
		srv_handle.ptr += dx.srv_descriptor_size * dx_world.current_image_indices[0];
		dx.command_list->SetGraphicsRootDescriptorTable(1, srv_handle);

		D3D12_GPU_DESCRIPTOR_HANDLE sampler_handle = dx.sampler_heap->GetGPUDescriptorHandleForHeapStart();
		const int sampler_index = dx_world.images[dx_world.current_image_indices[0]].sampler_index;
		sampler_handle.ptr += dx.sampler_descriptor_size * sampler_index;
		dx.command_list->SetGraphicsRootDescriptorTable(2, sampler_handle);
	}

	if (multitexture) {
		D3D12_GPU_DESCRIPTOR_HANDLE srv_handle = dx.srv_heap->GetGPUDescriptorHandleForHeapStart();
		srv_handle.ptr += dx.srv_descriptor_size * dx_world.current_image_indices[1];
		dx.command_list->SetGraphicsRootDescriptorTable(3, srv_handle);

		D3D12_GPU_DESCRIPTOR_HANDLE sampler_handle = dx.sampler_heap->GetGPUDescriptorHandleForHeapStart();
		const int sampler_index = dx_world.images[dx_world.current_image_indices[1]].sampler_index;
		sampler_handle.ptr += dx.sampler_descriptor_size * sampler_index;
		dx.command_list->SetGraphicsRootDescriptorTable(4, sampler_handle);
	}

	//
	// Configure pipeline.
	//
	dx.command_list->SetPipelineState(pipeline);
	dx.command_list->IASetPrimitiveTopology(lines ? D3D10_PRIMITIVE_TOPOLOGY_LINELIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	D3D12_RECT scissor_rect = get_scissor_rect();
	dx.command_list->RSSetScissorRects(1, &scissor_rect);

	D3D12_VIEWPORT viewport = get_viewport(depth_range);
	dx.command_list->RSSetViewports(1, &viewport);

	//
	// Draw.
	//
	if (indexed)
		dx.command_list->DrawIndexedInstanced(tess.numIndexes, 1, 0, 0, 0);
	else
		dx.command_list->DrawInstanced(tess.numVertexes, 1, 0, 0);
}

void dx_begin_frame() {
	if (!dx.active)
		return;

	if (dx.fence->GetCompletedValue() < dx.fence_value) {
		DX_CHECK(dx.fence->SetEventOnCompletion(dx.fence_value, dx.fence_event))
		WaitForSingleObject(dx.fence_event, INFINITE);
	}

	dx.frame_index = dx.swapchain->GetCurrentBackBufferIndex();

	DX_CHECK(dx.command_allocator->Reset())
	DX_CHECK(dx.command_list->Reset(dx.command_allocator, nullptr))

	dx.command_list->SetGraphicsRootSignature(dx.root_signature);

	ID3D12DescriptorHeap* heaps[] = { dx.srv_heap, dx.sampler_heap };
	dx.command_list->SetDescriptorHeaps(_countof(heaps), heaps);

	D3D12_RESOURCE_BARRIER transition_barrier = get_transition_barrier(dx.render_targets[dx.frame_index],
																	   D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

	dx.command_list->ResourceBarrier(1, &transition_barrier);

	D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle = dx.dsv_heap->GetCPUDescriptorHandleForHeapStart();

	D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = dx.rtv_heap->GetCPUDescriptorHandleForHeapStart();
	rtv_handle.ptr += dx.frame_index * dx.rtv_descriptor_size;

	dx.command_list->OMSetRenderTargets(1, &rtv_handle, FALSE, &dsv_handle);

	dx.xyz_elements = 0;
	dx.color_st_elements = 0;
	dx.index_buffer_offset = 0;
}

void dx_end_frame() {
	if (!dx.active)
		return;

	D3D12_RESOURCE_BARRIER transition_barrier = get_transition_barrier(dx.render_targets[dx.frame_index],
																	   D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

	dx.command_list->ResourceBarrier(1, &transition_barrier);

	DX_CHECK(dx.command_list->Close())

	ID3D12CommandList* command_list = dx.command_list;
	dx.command_queue->ExecuteCommandLists(1, &command_list);

	dx.fence_value++;
	DX_CHECK(dx.command_queue->Signal(dx.fence, dx.fence_value))

	DX_CHECK(dx.swapchain->Present(0, 0))
}

#else // ENABLE_DX12

void dx_initialize() {}
void dx_shutdown() {}
void dx_release_resources() {}
void dx_wait_device_idle() {}

Dx_Image dx_create_image(int width, int height, Dx_Image_Format format, int mip_levels,  bool repeat_texture, int image_index) { return Dx_Image{}; }
void dx_upload_image_data(ID3D12Resource* texture, int width, int height, int mip_levels, const uint8_t* pixels, int bytes_per_pixel) {}
void dx_create_sampler_descriptor(const Vk_Sampler_Def& def, Dx_Sampler_Index sampler_index) {}
ID3D12PipelineState* dx_find_pipeline(const Vk_Pipeline_Def& def) { return nullptr; }

void dx_clear_attachments(bool clear_depth_stencil, bool clear_color, vec4_t color) {}
void dx_bind_geometry() {}
void dx_shade_geometry(ID3D12PipelineState* pipeline, bool multitexture, Vk_Depth_Range depth_range, bool indexed, bool lines) {}
void dx_begin_frame() {}
void dx_end_frame() {}

#endif  // ENABLE_DX12
