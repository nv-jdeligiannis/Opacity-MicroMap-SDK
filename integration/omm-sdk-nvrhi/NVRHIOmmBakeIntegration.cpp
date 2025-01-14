/*
Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.

NVIDIA CORPORATION and its licensors retain all intellectual property
and proprietary rights in and to this software, related documentation
and any modifications thereto. Any use, reproduction, disclosure or
distribution of this software and related documentation without an express
license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#include <omm-sdk-nvrhi/NVRHIOmmBakeIntegration.h>
#include <nvrhi/common/misc.h>
#include <nvrhi/utils.h>

constexpr uint32_t g_DebugRTVDimension =  6 * 1024;

namespace
{

	nvrhi::SamplerAddressMode GetNVRHIAddressMode(const omm::TextureAddressMode addressingMode)
	{
		switch (addressingMode)
		{
		case omm::TextureAddressMode::Wrap: return nvrhi::SamplerAddressMode::Wrap;
		case omm::TextureAddressMode::Mirror: return nvrhi::SamplerAddressMode::Mirror;
		case omm::TextureAddressMode::Clamp: return nvrhi::SamplerAddressMode::Clamp;
		case omm::TextureAddressMode::Border: return nvrhi::SamplerAddressMode::Border;
		case omm::TextureAddressMode::MirrorOnce: return nvrhi::SamplerAddressMode::MirrorOnce;
		default:
		{
			assert(false);
			return nvrhi::SamplerAddressMode::Clamp;
		}
		}
	}

	omm::TextureAddressMode GetTextureAddressMode(const nvrhi::SamplerAddressMode addressingMode)
	{
		switch (addressingMode)
		{
		case nvrhi::SamplerAddressMode::Wrap: return omm::TextureAddressMode::Wrap;
		case nvrhi::SamplerAddressMode::Mirror: return omm::TextureAddressMode::Mirror;
		case nvrhi::SamplerAddressMode::Clamp: return omm::TextureAddressMode::Clamp;
		case nvrhi::SamplerAddressMode::Border: return omm::TextureAddressMode::Border;
		case nvrhi::SamplerAddressMode::MirrorOnce: return omm::TextureAddressMode::MirrorOnce;
		default:
		{
			assert(false);
			return omm::TextureAddressMode::Clamp;
		}
		}
	}
}

/// -- BINDING CACHE FROM DONUT -- 
#include <unordered_map>
#include <list>
#include <shared_mutex>

/*
BindingCache maintains a dictionary that maps binding set descriptors
into actual binding set objects. The binding sets are created on demand when
GetOrCreateBindingSet(...) is called and the requested binding set does not exist.
Created binding sets are stored for the lifetime of BindingCache, or until
Clear() is called.

All BindingCache methods are thread-safe.
*/
class BindingCache
{
private:
	nvrhi::DeviceHandle m_Device;
	std::unordered_map<size_t, nvrhi::BindingSetHandle> m_BindingSets;
	std::shared_mutex m_Mutex;

public:
	BindingCache(nvrhi::IDevice* device)
		: m_Device(device)
	{ }

	nvrhi::BindingSetHandle GetCachedBindingSet(const nvrhi::BindingSetDesc& desc, nvrhi::IBindingLayout* layout)
	{
		size_t hash = 0;
		nvrhi::hash_combine(hash, desc);
		nvrhi::hash_combine(hash, layout);

		m_Mutex.lock_shared();

		nvrhi::BindingSetHandle result = nullptr;
		auto it = m_BindingSets.find(hash);
		if (it != m_BindingSets.end())
			result = it->second;

		m_Mutex.unlock_shared();

		if (result)
		{
			assert(result->getDesc());
			assert(*result->getDesc() == desc);
		}

		return result;
	}
	nvrhi::BindingSetHandle GetOrCreateBindingSet(const nvrhi::BindingSetDesc& desc, nvrhi::IBindingLayout* layout)
	{
		size_t hash = 0;
		nvrhi::hash_combine(hash, desc);
		nvrhi::hash_combine(hash, layout);

		m_Mutex.lock_shared();

		nvrhi::BindingSetHandle result;
		auto it = m_BindingSets.find(hash);
		if (it != m_BindingSets.end())
			result = it->second;

		m_Mutex.unlock_shared();

		if (!result)
		{
			m_Mutex.lock();

			nvrhi::BindingSetHandle& entry = m_BindingSets[hash];
			if (!entry)
			{
				result = m_Device->createBindingSet(desc, layout);
				entry = result;
			}
			else
				result = entry;

			m_Mutex.unlock();
		}

		if (result)
		{
			assert(result->getDesc());
			assert(*result->getDesc() == desc);
		}

		return result;
	}
	void Clear();
};

NVRHIVmBakeIntegration::NVRHIVmBakeIntegration(nvrhi::DeviceHandle device, nvrhi::CommandListHandle commandList, bool enableDebug)
	: m_device(device)
	, m_bindingCache(new BindingCache(device))
	, m_enableDebug(enableDebug)
{
	InitStaticBuffers(commandList);
	InitBaker();
}

NVRHIVmBakeIntegration::~NVRHIVmBakeIntegration()
{
	delete m_bindingCache;
	m_bindingCache = nullptr;
	DestroyBaker();
}

void NVRHIVmBakeIntegration::InitStaticBuffers(nvrhi::CommandListHandle commandList)
{
	{
		size_t size = 0;
		omm::Result res = omm::Gpu::GetStaticResourceData(omm::Gpu::ResourceType::STATIC_VERTEX_BUFFER, nullptr, size);
		assert(res == omm::Result::SUCCESS);

		std::vector<uint8_t> vertexData(size);
		res = omm::Gpu::GetStaticResourceData(omm::Gpu::ResourceType::STATIC_VERTEX_BUFFER, vertexData.data(), size);
		assert(res == omm::Result::SUCCESS);

		nvrhi::BufferDesc bufferDesc;
		bufferDesc.isVertexBuffer = true;
		bufferDesc.byteSize = vertexData.size();
		bufferDesc.debugName = "omm::Gpu::ResourceType::STATIC_VERTEX_BUFFER";
		bufferDesc.format = nvrhi::Format::R32_UINT;
		m_staticVertexBuffer = m_device->createBuffer(bufferDesc);

		commandList->beginTrackingBufferState(m_staticVertexBuffer, nvrhi::ResourceStates::Common);
		commandList->writeBuffer(m_staticVertexBuffer, vertexData.data(), vertexData.size());
		commandList->setPermanentBufferState(m_staticVertexBuffer, nvrhi::ResourceStates::VertexBuffer);
	}

	{
		size_t size = 0;
		omm::Result res = omm::Gpu::GetStaticResourceData(omm::Gpu::ResourceType::STATIC_INDEX_BUFFER, nullptr, size);
		assert(res == omm::Result::SUCCESS);

		std::vector<uint8_t> indexData(size);
		res = omm::Gpu::GetStaticResourceData(omm::Gpu::ResourceType::STATIC_INDEX_BUFFER, indexData.data(), size);
		assert(res == omm::Result::SUCCESS);

		nvrhi::BufferDesc bufferDesc;
		bufferDesc.isIndexBuffer = true;
		bufferDesc.byteSize = indexData.size();
		bufferDesc.debugName = "omm::Gpu::ResourceType::STATIC_INDEX_BUFFER";
		bufferDesc.format = nvrhi::Format::R32_UINT;
		m_staticIndexBuffer = m_device->createBuffer(bufferDesc);

		commandList->beginTrackingBufferState(m_staticIndexBuffer, nvrhi::ResourceStates::Common);
		commandList->writeBuffer(m_staticIndexBuffer, indexData.data(), indexData.size());
		commandList->setPermanentBufferState(m_staticIndexBuffer, nvrhi::ResourceStates::IndexBuffer);
	}

	{ // NVRHI has trouble binding zero RTV's
		nvrhi::TextureHandle virtualTexture;
		{
			nvrhi::TextureDesc desc;
			desc.debugName = "NULL_VMRT";
			desc.width = m_enableDebug ? g_DebugRTVDimension : 1;
			desc.height = m_enableDebug ? g_DebugRTVDimension : 1;
			desc.format = nvrhi::Format::RGBA16_FLOAT;
			desc.dimension = nvrhi::TextureDimension::Texture2D;
			desc.clearValue = nvrhi::Color();
			desc.useClearValue = true;
			desc.isRenderTarget = true;
			desc.isVirtual = !m_enableDebug;
			virtualTexture = m_device->createTexture(desc);
		}

		{
			nvrhi::FramebufferDesc desc;
			nvrhi::FramebufferAttachment tex;
			tex.format = nvrhi::Format::RGBA16_FLOAT;
			tex.setTexture(virtualTexture);
			desc.addColorAttachment(tex);
			m_nullFbo = m_device->createFramebuffer(desc);
		}
	}
}

void NVRHIVmBakeIntegration::ReserveGlobalCBuffer(size_t byteSize, uint32_t slot)
{
	if (!m_globalCBuffer.Get() || m_globalCBuffer->getDesc().byteSize < byteSize)
	{
		m_globalCBuffer = m_device->createBuffer(nvrhi::utils::CreateStaticConstantBufferDesc((uint32_t)byteSize, "omm::Gpu::GlobalConstantBuffer"));
	}

	m_globalCBufferSlot = slot;
}

void NVRHIVmBakeIntegration::InitBaker()
{
	assert(m_device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12 || m_device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN);

	{
		omm::BakerCreationDesc desc;
		desc.type = omm::BakerType::GPU;
		desc.enableValidation = true;

		omm::Result res = omm::CreateOpacityMicromapBaker(desc, &m_baker);
		assert(res == omm::Result::SUCCESS);
	}

	{
		omm::BakerCreationDesc desc;
		desc.type = omm::BakerType::CPU;
		desc.enableValidation = true;

		omm::Result res = omm::CreateOpacityMicromapBaker(desc, &m_cpuBaker);
		assert(res == omm::Result::SUCCESS);
	}

	{
		omm::Gpu::BakePipelineConfigDesc config;
		config.renderAPI = m_device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12 ? omm::Gpu::RenderAPI::DX12 : omm::Gpu::RenderAPI::Vulkan;

		omm::Result res = omm::Gpu::CreatePipeline(m_baker, config, &m_pipeline);
		assert(res == omm::Result::SUCCESS);

		const omm::Gpu::BakePipelineInfoDesc* desc;
		res = omm::Gpu::GetPipelineDesc(m_pipeline, desc);
		assert(res == omm::Result::SUCCESS);

		SetupPipelines(desc);

		ReserveGlobalCBuffer(desc->globalConstantBufferDesc.maxDataSize, desc->globalConstantBufferDesc.registerIndex);
		m_localCBufferSlot = desc->localConstantBufferDesc.registerIndex;
		m_localCBufferSize = desc->localConstantBufferDesc.maxDataSize;
	}
}

void NVRHIVmBakeIntegration::DestroyBaker()
{
	omm::Result res = omm::Gpu::DestroyPipeline(m_baker, m_pipeline);
	assert(res == omm::Result::SUCCESS);

	res = omm::DestroyOpacityMicromapBaker(m_baker);
	assert(res == omm::Result::SUCCESS);

	res = omm::DestroyOpacityMicromapBaker(m_cpuBaker);
	assert(res == omm::Result::SUCCESS);
}


void NVRHIVmBakeIntegration::SetupPipelines(
	const omm::Gpu::BakePipelineInfoDesc* desc)
{
	auto CreateBindingLayout = [this, desc](
		nvrhi::ShaderType visibility, 
		const omm::Gpu::DescriptorRangeDesc* ranges, uint32_t numRanges)->nvrhi::BindingLayoutHandle {
		nvrhi::VulkanBindingOffsets bindingOffsets;
		bindingOffsets.shaderResource = desc->spirvBindingOffsets.textureOffset;
		bindingOffsets.sampler = desc->spirvBindingOffsets.samplerOffset;
		bindingOffsets.constantBuffer = desc->spirvBindingOffsets.constantBufferOffset;
		bindingOffsets.unorderedAccess = desc->spirvBindingOffsets.storageTextureAndBufferOffset;

		nvrhi::BindingLayoutDesc layoutDesc;
		layoutDesc.visibility = visibility;
		layoutDesc.bindingOffsets = bindingOffsets;

		nvrhi::BindingLayoutItem constantBufferItem = nvrhi::BindingLayoutItem::ConstantBuffer(desc->globalConstantBufferDesc.registerIndex);
		layoutDesc.bindings.push_back(constantBufferItem);

		nvrhi::BindingLayoutItem pushConstantBufferItem = nvrhi::BindingLayoutItem::PushConstants(
			desc->localConstantBufferDesc.registerIndex, desc->localConstantBufferDesc.maxDataSize);
		layoutDesc.bindings.push_back(pushConstantBufferItem);

		for (uint32_t samplerIt = 0; samplerIt < desc->staticSamplersNum; ++samplerIt)
		{
			const omm::Gpu::StaticSamplerDesc& sampler = desc->staticSamplers[samplerIt];
			layoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Sampler(sampler.registerIndex));
		}

		for (uint32_t descriptorRangeIndex = 0; descriptorRangeIndex < numRanges; descriptorRangeIndex++)
		{
			const omm::Gpu::DescriptorRangeDesc& descriptorRange = ranges[descriptorRangeIndex];

			nvrhi::BindingLayoutItem resourceItem = {};
			switch (descriptorRange.descriptorType)
			{
			case omm::Gpu::DescriptorType::TextureRead:
				resourceItem.type = nvrhi::ResourceType::Texture_SRV;
				break;
			case omm::Gpu::DescriptorType::RawBufferRead:
				resourceItem.type = nvrhi::ResourceType::RawBuffer_SRV;
				break;
			case omm::Gpu::DescriptorType::RawBufferWrite:
				resourceItem.type = nvrhi::ResourceType::RawBuffer_UAV;
				break;
			case omm::Gpu::DescriptorType::BufferRead:
				resourceItem.type = nvrhi::ResourceType::TypedBuffer_SRV;
				break;
			default:
				assert(!"Unknown NRD descriptor type");
				break;
			}

			for (uint32_t descriptorOffset = 0; descriptorOffset < descriptorRange.descriptorNum; descriptorOffset++)
			{
				resourceItem.slot = descriptorRange.baseRegisterIndex + descriptorOffset;
				layoutDesc.bindings.push_back(resourceItem);
			}
		}

		return m_device->createBindingLayout(layoutDesc);
	};

	for (uint32_t samplerIt = 0; samplerIt < desc->staticSamplersNum; ++samplerIt)
	{
		const omm::SamplerDesc& samplerDesc = desc->staticSamplers[samplerIt].desc;
		nvrhi::SamplerDesc sDesc;
		sDesc.setAllFilters(samplerDesc.filter == omm::TextureFilterMode::Linear);
		sDesc.setAllAddressModes(GetNVRHIAddressMode(samplerDesc.addressingMode));
		m_samplers.push_back(std::make_pair(m_device->createSampler(sDesc), desc->staticSamplers[samplerIt].registerIndex));
	}

	for (uint32_t pipelineIt = 0; pipelineIt < desc->pipelineNum; ++pipelineIt)
	{
		const omm::Gpu::PipelineDesc& pipeline = desc->pipelines[pipelineIt];

		switch (pipeline.type)
		{
		case omm::Gpu::PipelineType::Compute:
		{
			const omm::Gpu::ComputePipelineDesc& compute = pipeline.compute;

			nvrhi::ShaderHandle shader;
			{
				nvrhi::ShaderDesc shaderDesc(nvrhi::ShaderType::Compute);
				shaderDesc.debugName = compute.shaderFileName;
				shaderDesc.entryName = compute.shaderEntryPointName;
				shader = m_device->createShader(shaderDesc, compute.computeShader.data, compute.computeShader.size);
			}

			nvrhi::BindingLayoutHandle layout = CreateBindingLayout(nvrhi::ShaderType::Compute, 
				compute.descriptorRanges, compute.descriptorRangeNum);
			
			nvrhi::ComputePipelineHandle pipeline;
			{
				nvrhi::ComputePipelineDesc csDesc;
				csDesc.CS = shader;
				csDesc.bindingLayouts = { layout };
				pipeline = m_device->createComputePipeline(csDesc);
			}
			m_pipelines.push_back(pipeline);
			break;
		}
		case omm::Gpu::PipelineType::Graphics:
		{
			const omm::Gpu::GraphicsPipelineDesc& gfx = pipeline.graphics;
			static_assert(omm::Gpu::GraphicsPipelineDesc::VERSION == 1, "New GFX pipeline version detected, update integration code.");

			nvrhi::ShaderHandle vertex;
			{
				nvrhi::ShaderDesc shaderDesc(nvrhi::ShaderType::Vertex);
				shaderDesc.debugName = gfx.vertexShaderFileName;
				shaderDesc.entryName = gfx.vertexShaderEntryPointName;
				vertex = m_device->createShader(shaderDesc, gfx.vertexShader.data, gfx.vertexShader.size);
			}

			nvrhi::ShaderHandle geometry;
			if (gfx.geometryShaderFileName)
			{
				nvrhi::ShaderDesc shaderDesc(nvrhi::ShaderType::Geometry);
				shaderDesc.debugName = gfx.geometryShaderFileName;
				shaderDesc.entryName = gfx.geometryShaderEntryPointName;
				geometry = m_device->createShader(shaderDesc, gfx.geometryShader.data, gfx.geometryShader.size);
			}

			nvrhi::ShaderHandle pixel;
			{
				nvrhi::ShaderDesc shaderDesc(nvrhi::ShaderType::Pixel);
				shaderDesc.debugName = gfx.pixelShaderFileName;
				shaderDesc.entryName = gfx.pixelShaderEntryPointName;
				pixel = m_device->createShader(shaderDesc, gfx.pixelShader.data, gfx.pixelShader.size);
			}

			nvrhi::BindingLayoutHandle layout = CreateBindingLayout(nvrhi::ShaderType::AllGraphics, gfx.descriptorRanges, gfx.descriptorRangeNum);

			nvrhi::InputLayoutHandle inputLayout;
			{
				nvrhi::VertexAttributeDesc desc;
				desc.name = omm::Gpu::GraphicsPipelineDesc::InputElementDesc::semanticName;
				desc.format = nvrhi::Format::R32_UINT;
				desc.elementStride = sizeof(uint32_t);
				static_assert(omm::Gpu::GraphicsPipelineDesc::InputElementDesc::format == omm::Gpu::BufferFormat::R32_UINT);
				desc.arraySize = 1;
				static_assert(omm::Gpu::GraphicsPipelineDesc::inputElementDescCount == 1);
				desc.bufferIndex = 0;
				static_assert(omm::Gpu::GraphicsPipelineDesc::InputElementDesc::inputSlot == 0);
				desc.offset = 0;
				static_assert(omm::Gpu::GraphicsPipelineDesc::InputElementDesc::semanticIndex == 0);
				inputLayout = m_device->createInputLayout(&desc, 1 /*attributeCount*/, vertex);
			}

			nvrhi::GraphicsPipelineHandle pipeline;
			{
				static_assert(omm::Gpu::GraphicsPipelineDesc::RasterState::cullMode == omm::Gpu::RasterCullMode::None);
				static_assert(omm::Gpu::GraphicsPipelineDesc::topology == omm::Gpu::PrimitiveTopology::TriangleList);
				static_assert(omm::Gpu::GraphicsPipelineDesc::DepthState::depthTestEnable == false);
				static_assert(omm::Gpu::GraphicsPipelineDesc::DepthState::depthWriteEnable == false);
				static_assert(omm::Gpu::GraphicsPipelineDesc::DepthState::stencilEnable == false);

				nvrhi::GraphicsPipelineDesc gfxDesc;
				gfxDesc.primType = nvrhi::PrimitiveType::TriangleList;
				gfxDesc.renderState.depthStencilState.disableDepthTest();
				gfxDesc.renderState.depthStencilState.disableDepthWrite();
				gfxDesc.renderState.depthStencilState.disableStencil();
				gfxDesc.VS = vertex;
				gfxDesc.GS = geometry;
				gfxDesc.PS = pixel;
				gfxDesc.bindingLayouts = { layout };
				gfxDesc.inputLayout = inputLayout;
				gfxDesc.renderState.rasterState.conservativeRasterEnable = gfx.rasterState.conservativeRasterization;
				gfxDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
				gfxDesc.renderState.rasterState.frontCounterClockwise = true;
				gfxDesc.renderState.rasterState.enableScissor(); // <- This is to prevent the framebuffer from implicitly setting the scissor rect...
				pipeline = m_device->createGraphicsPipeline(gfxDesc, m_nullFbo);
			}
			m_pipelines.push_back(pipeline);
			break;
		}
		default:
		{
			assert(false);
			break;
		}
		}
	}
}

omm::Gpu::BakeDispatchConfigDesc NVRHIVmBakeIntegration::GetConfig(const Input& params)
{
	using namespace omm;

	Gpu::BakeDispatchConfigDesc config;
	config.runtimeSamplerDesc.addressingMode	= GetTextureAddressMode(params.sampleMode);
	config.runtimeSamplerDesc.filter			= params.bilinearFilter ? TextureFilterMode::Linear : TextureFilterMode::Nearest;
	if (m_enableDebug)
		config.bakeFlags = (omm::Gpu::BakeFlags)((uint32_t)config.bakeFlags | (uint32_t)omm::Gpu::BakeFlags::EnableNsightDebugMode);
	config.bakeFlags = (omm::Gpu::BakeFlags)((uint32_t)config.bakeFlags | (uint32_t)omm::Gpu::BakeFlags::EnablePostBuildInfo);
	
	if (!params.enableSpecialIndices)
		config.bakeFlags = (omm::Gpu::BakeFlags)((uint32_t)config.bakeFlags | (uint32_t)omm::Gpu::BakeFlags::DisableSpecialIndices);

	if (params.force32BitIndices)
		config.bakeFlags = (omm::Gpu::BakeFlags)((uint32_t)config.bakeFlags | (uint32_t)omm::Gpu::BakeFlags::Force32BitIndices);

	if (!params.enableTexCoordDeuplication)
		config.bakeFlags = (omm::Gpu::BakeFlags)((uint32_t)config.bakeFlags | (uint32_t)omm::Gpu::BakeFlags::DisableTexCoordDeduplication);

	if (params.computeOnly)
		config.bakeFlags = (omm::Gpu::BakeFlags)((uint32_t)config.bakeFlags | (uint32_t)omm::Gpu::BakeFlags::ComputeOnly);

	config.alphaTextureWidth					= params.alphaTexture ? params.alphaTexture->getDesc().width : 1;
	config.alphaTextureHeight					= params.alphaTexture ? params.alphaTexture->getDesc().height : 1;
	config.alphaTextureChannel					= params.alphaTextureChannel;
	config.alphaMode							= AlphaMode::Test;
	config.alphaCutoff							= params.alphaCutoff;
	config.texCoordFormat						= TexCoordFormat::UV32_FLOAT;
	config.texCoordOffsetInBytes				= params.texCoordBufferOffsetInBytes;
	config.texCoordStrideInBytes				= params.texCoordStrideInBytes;
	config.indexFormat							= IndexFormat::I32_UINT;
	config.indexCount							= (uint32_t)params.numIndices;
	config.globalOMMFormat						= params.use2State ? OMMFormat::OC1_2_State : OMMFormat::OC1_4_State;
	config.supportedOMMFormats[0]				= params.use2State ? OMMFormat::OC1_2_State : OMMFormat::OC1_4_State;
	config.numSupportedOMMFormats				= 1;
	config.maxScratchMemorySize					= params.minimalMemoryMode ? Gpu::ScratchMemoryBudget::MB_4 : Gpu::ScratchMemoryBudget::HighMemory;
	config.maxSubdivisionLevel					= params.globalSubdivisionLevel;
	config.globalSubdivisionLevel				= params.globalSubdivisionLevel;
	config.dynamicSubdivisionScale				= params.dynamicSubdivisionScale;
	return config;
}

void NVRHIVmBakeIntegration::ReserveScratchBuffers(const omm::Gpu::PreBakeInfo& info)
{
	for (uint32_t poolIt = 0; poolIt < info.numTransientPoolBuffers; poolIt++)
	{
		if (m_transientPool.size() <= poolIt)
		{
			m_transientPool.push_back(nullptr);
		}

		const size_t bufferSize = info.transientPoolBufferSizeInBytes[poolIt];

		if (m_transientPool[poolIt] == nullptr || m_transientPool[poolIt]->getDesc().byteSize < bufferSize)
		{
			nvrhi::BufferDesc bufferDesc;
			bufferDesc.byteSize = bufferSize;
			bufferDesc.debugName = "omm::Gpu::ResourceType::TRANSIENT_POOL_" + std::to_string(poolIt);
			bufferDesc.format = nvrhi::Format::R32_UINT;
			bufferDesc.canHaveUAVs = true;
			bufferDesc.canHaveRawViews = true;
			bufferDesc.isDrawIndirectArgs = true;
			m_transientPool[poolIt] = m_device->createBuffer(bufferDesc);
		}
	}
}

void NVRHIVmBakeIntegration::GetPreBakeInfo(const Input& params, PreBakeInfo& info)
{
	using namespace omm;

	Gpu::BakeDispatchConfigDesc config = GetConfig(params);

	Gpu::PreBakeInfo preBuildInfo;
	omm::Result res = Gpu::GetPreBakeInfo(m_pipeline, config, &preBuildInfo);
	assert(res == omm::Result::SUCCESS);

	info.ommIndexFormat = preBuildInfo.outOmmIndexBufferFormat == omm::IndexFormat::I16_UINT ? nvrhi::Format::R16_UINT : nvrhi::Format::R32_UINT;
	info.ommIndexBufferSize = preBuildInfo.outOmmIndexBufferSizeInBytes;
	info.ommIndexHistogramSize = preBuildInfo.outOmmIndexHistogramSizeInBytes;
	info.ommIndexCount = preBuildInfo.outOmmIndexCount;
	info.ommArrayBufferSize = preBuildInfo.outOmmArraySizeInBytes;
	info.ommDescBufferSize = preBuildInfo.outOmmDescSizeInBytes;
	info.ommDescArrayHistogramSize = preBuildInfo.outOmmArrayHistogramSizeInBytes;
	info.ommPostBuildInfoBufferSize = preBuildInfo.outOmmPostBuildInfoSizeInBytes;
}

void NVRHIVmBakeIntegration::RunBake(
	nvrhi::CommandListHandle commandList,
	const Input& params, 
	const Output& result)
{
	using namespace omm;

	Gpu::BakeDispatchConfigDesc config = GetConfig(params);

	Gpu::PreBakeInfo preBuildInfo;
	omm::Result res = Gpu::GetPreBakeInfo(m_pipeline, config, &preBuildInfo);
	assert(res == omm::Result::SUCCESS);

	ReserveScratchBuffers(preBuildInfo);

	const omm::Gpu::BakeDispatchChain* dispatchDesc = nullptr;
	res = Gpu::Bake(m_pipeline, config, dispatchDesc);
	assert(res == omm::Result::SUCCESS);

	ExecuteBakeOperation(commandList, params, result, dispatchDesc);
}

void NVRHIVmBakeIntegration::ReadPostBuildInfo(void* pData, size_t byteSize, PostBuildInfo& outPostBuildInfo)
{
	static_assert(sizeof(omm::Gpu::PostBakeInfo) == sizeof(PostBuildInfo));
	assert(byteSize >= sizeof(PostBuildInfo));
	memcpy(&outPostBuildInfo, pData, sizeof(PostBuildInfo));
}

void NVRHIVmBakeIntegration::ReadUsageDescBuffer(void* pData, size_t byteSize, std::vector<OpacityMicromapUsageCount>& outVmUsages)
{
	omm::Cpu::OpacityMicromapUsageCount* usageDescs = static_cast<omm::Cpu::OpacityMicromapUsageCount*>(pData);
	const size_t usageDescNum = byteSize / sizeof(omm::Cpu::OpacityMicromapUsageCount);
	std::vector<OpacityMicromapUsageCount> vmUsages;
	for (uint32_t i = 0; i < usageDescNum; ++i) {
		if (usageDescs[i].count != 0)
		{
			OpacityMicromapUsageCount desc;
			desc.count = usageDescs[i].count;
			desc.format = usageDescs[i].format;
			desc.subdivisionLevel = usageDescs[i].subdivisionLevel;
			outVmUsages.push_back(desc);
		}
	}
}

nvrhi::TextureHandle NVRHIVmBakeIntegration::GetTextureResource(const Input& params, const Output& output, const omm::Gpu::Resource& resource)
{
	nvrhi::TextureHandle resourceHandle;
	switch (resource.type)
	{
	case omm::Gpu::ResourceType::IN_ALPHA_TEXTURE:
		resourceHandle = params.alphaTexture;
		break;
	default:
		assert(!"Unavailable resource type");
		break;
	}

	assert(resourceHandle);

	return resourceHandle;
}

nvrhi::BufferHandle NVRHIVmBakeIntegration::GetBufferResource(
	const Input& params, 
	const Output& output, 
	const omm::Gpu::Resource& resource,
	uint32_t& offsetInBytes)
{
	offsetInBytes = 0;
	nvrhi::BufferHandle resourceHandle;
	switch (resource.type)
	{
	case omm::Gpu::ResourceType::OUT_OMM_ARRAY_DATA:
		resourceHandle = output.ommArrayBuffer;
		break;
	case omm::Gpu::ResourceType::OUT_OMM_DESC_ARRAY:
		resourceHandle = output.ommDescBuffer;
		break;
	case omm::Gpu::ResourceType::OUT_OMM_INDEX_BUFFER:
		resourceHandle = output.ommIndexBuffer;
		break;
	case omm::Gpu::ResourceType::OUT_OMM_DESC_ARRAY_HISTOGRAM:
		resourceHandle = output.ommDescArrayHistogramBuffer;
		break;
	case omm::Gpu::ResourceType::OUT_OMM_INDEX_HISTOGRAM:
		resourceHandle = output.ommIndexHistogramBuffer;
		break;
	case omm::Gpu::ResourceType::OUT_POST_BAKE_INFO:
		resourceHandle = output.ommPostBuildInfoBuffer;
		break;
	case omm::Gpu::ResourceType::IN_INDEX_BUFFER:
		resourceHandle = params.indexBuffer;
		offsetInBytes = params.indexBufferOffsetInBytes;
		break;
	case omm::Gpu::ResourceType::IN_TEXCOORD_BUFFER:
		resourceHandle = params.texCoordBuffer;
		break;
	case omm::Gpu::ResourceType::TRANSIENT_POOL_BUFFER:
		resourceHandle = m_transientPool[resource.indexInPool];
		break;
	case omm::Gpu::ResourceType::STATIC_INDEX_BUFFER:
		resourceHandle = m_staticIndexBuffer;
		break;
	case omm::Gpu::ResourceType::STATIC_VERTEX_BUFFER:
		resourceHandle = m_staticVertexBuffer;
		break;
	default:
		assert(!"Unavailable resource type");
		break;
	}

	assert(resourceHandle);

	return resourceHandle;
}

void NVRHIVmBakeIntegration::ExecuteBakeOperation(
	nvrhi::CommandListHandle commandList,
	const Input& params, 
	const Output& output,
	const omm::Gpu::BakeDispatchChain* dispatchDesc)
{
	nvrhi::TextureHandle rtv = m_enableDebug ? m_nullFbo->getDesc().colorAttachments[0].texture : nullptr;

	commandList->beginTrackingBufferState(m_globalCBuffer, nvrhi::ResourceStates::ConstantBuffer);
	
	if (rtv)
		commandList->beginTrackingTextureState(rtv, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
	for (auto it : m_transientPool)
		commandList->beginTrackingBufferState(it, nvrhi::ResourceStates::Common);

	auto CreateDescriptorRangeDesc = [this, &output, &params](
		nvrhi::CommandListHandle commandList,
		const omm::Gpu::Resource* resources, uint32_t numResources, 
		const omm::Gpu::DescriptorRangeDesc* ranges, uint32_t numRanges)->nvrhi::BindingSetDesc {

		nvrhi::BindingSetDesc setDesc;

		commandList->setBufferState(m_globalCBuffer, nvrhi::ResourceStates::ConstantBuffer);
		setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(m_globalCBufferSlot, m_globalCBuffer));
		setDesc.addItem(nvrhi::BindingSetItem::PushConstants(m_localCBufferSlot, m_localCBufferSize));

		for (uint32_t i = 0; i < m_samplers.size(); ++i)
		{
			setDesc.addItem(nvrhi::BindingSetItem::Sampler(m_samplers[i].second, m_samplers[i].first.Get()));
		}

		uint32_t resourceIndex = 0;
		//
		for (uint32_t descriptorRangeIndex = 0; descriptorRangeIndex < numRanges; descriptorRangeIndex++)
		{
			const omm::Gpu::DescriptorRangeDesc& descriptorRange = ranges[descriptorRangeIndex];

			for (uint32_t descriptorOffset = 0; descriptorOffset < descriptorRange.descriptorNum; descriptorOffset++)
			{
				// assert(resourceIndex < dispatchDesc.resourceNum);
				const omm::Gpu::Resource& resource = resources[resourceIndex];
				assert(resource.stateNeeded == descriptorRange.descriptorType);

				const uint32_t slot = descriptorRange.baseRegisterIndex + descriptorOffset;
				
				if (descriptorRange.descriptorType == omm::Gpu::DescriptorType::TextureRead)
				{
					nvrhi::TextureSubresourceSet subresources = nvrhi::AllSubresources;
					subresources.baseMipLevel = resource.mipOffset;
					subresources.numMipLevels = resource.mipNum;
					nvrhi::TextureHandle buffer = GetTextureResource(params, output, resource);
					commandList->setTextureState(buffer, subresources, nvrhi::ResourceStates::ShaderResource);
					setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(slot, buffer));
				}
				else if (descriptorRange.descriptorType == omm::Gpu::DescriptorType::RawBufferRead)
				{
					uint32_t offset = 0;
					nvrhi::BufferHandle buffer = GetBufferResource(params, output, resource, offset);
					commandList->setBufferState(buffer, nvrhi::ResourceStates::ShaderResource);
					setDesc.addItem(nvrhi::BindingSetItem::RawBuffer_SRV(slot, buffer, nvrhi::BufferRange(offset, ~0ull)));
				}
				else if (descriptorRange.descriptorType == omm::Gpu::DescriptorType::RawBufferWrite)
				{
					uint32_t offset = 0;
					nvrhi::BufferHandle buffer = GetBufferResource(params, output, resource, offset);
					commandList->setBufferState(buffer, nvrhi::ResourceStates::UnorderedAccess);
					setDesc.addItem(nvrhi::BindingSetItem::RawBuffer_UAV(slot, buffer, nvrhi::BufferRange(offset, ~0ull)));
				}
				else if (descriptorRange.descriptorType == omm::Gpu::DescriptorType::BufferRead)
				{
					uint32_t offset = 0;
					nvrhi::BufferHandle buffer = GetBufferResource(params, output, resource, offset);
					commandList->setBufferState(buffer, nvrhi::ResourceStates::ShaderResource);
					setDesc.addItem(nvrhi::BindingSetItem::TypedBuffer_SRV(slot, buffer, buffer->getDesc().format, nvrhi::BufferRange(offset, ~0ull)));
				}
				else
				{
					assert(false);
				}
				resourceIndex++;
			}
		}
		return setDesc;
	};

	const omm::Gpu::BakePipelineInfoDesc* pipelineDesc;
	omm::Result res = omm::Gpu::GetPipelineDesc(m_pipeline, pipelineDesc);
	assert(res == omm::Result::SUCCESS);

	assert(m_globalCBuffer && m_globalCBuffer->getDesc().byteSize >= pipelineDesc->globalConstantBufferDesc.maxDataSize);

	if (dispatchDesc->globalCBufferDataSize != 0)
		commandList->writeBuffer(m_globalCBuffer, dispatchDesc->globalCBufferData, dispatchDesc->globalCBufferDataSize);
	
	auto SetPushConstants = [this](nvrhi::CommandListHandle commandList, const void* localConstantBufferData, size_t localConstantBufferDataSize) {
		if (m_localCBufferSize == 0)
			return;
		assert(m_localCBufferSize < 128);
		uint8_t* pushConstants = (uint8_t*)alloca(m_localCBufferSize);
		memset(pushConstants, 0, m_localCBufferSize);
		if (localConstantBufferDataSize != 0)
			memcpy(pushConstants, localConstantBufferData, localConstantBufferDataSize);
		commandList->setPushConstants(pushConstants, m_localCBufferSize);
	};

	for (uint32_t dispatchIt = 0; dispatchIt < dispatchDesc->numDispatches; ++dispatchIt)
	{
		const omm::Gpu::DispatchDesc& desc = dispatchDesc->dispatches[dispatchIt];

		switch (desc.type)
		{
		case omm::Gpu::DispatchType::BeginLabel:
		{
			const omm::Gpu::BeginLabelDesc& label = desc.beginLabel;
			std::string name = std::string("OMM:") + label.debugName;
			commandList->beginMarker(name.c_str());
		}
		break;
		case omm::Gpu::DispatchType::EndLabel:
		{
			commandList->endMarker();
		}
		break;
		case omm::Gpu::DispatchType::Compute:
		{
			const omm::Gpu::ComputeDesc& compute = desc.compute;
			const omm::Gpu::PipelineDesc& pipeline = pipelineDesc->pipelines[compute.pipelineIndex];

			assert(pipeline.type == omm::Gpu::PipelineType::Compute);

			nvrhi::BindingSetDesc setDesc = CreateDescriptorRangeDesc(
				commandList,
				compute.resources, compute.resourceNum, 
				pipeline.compute.descriptorRanges, pipeline.compute.descriptorRangeNum);
			nvrhi::IComputePipeline* csPipeline = nvrhi::checked_cast<nvrhi::IComputePipeline*>(m_pipelines[compute.pipelineIndex].Get());
			nvrhi::BindingSetHandle bindingSet = m_bindingCache->GetOrCreateBindingSet(setDesc, csPipeline->getDesc().bindingLayouts[0].Get());

			commandList->commitBarriers();

			nvrhi::ComputeState state;
			state.pipeline = csPipeline;
			state.bindings = { bindingSet };

			commandList->setComputeState(state);

			SetPushConstants(commandList, compute.localConstantBufferData, compute.localConstantBufferDataSize);

			commandList->dispatch(compute.gridWidth, compute.gridHeight, 1);
		}
		break;
		case omm::Gpu::DispatchType::ComputeIndirect:
		{
			const omm::Gpu::ComputeIndirectDesc& compute = desc.computeIndirect;
			const omm::Gpu::PipelineDesc& pipeline = pipelineDesc->pipelines[compute.pipelineIndex];

			assert(pipeline.type == omm::Gpu::PipelineType::Compute);

			nvrhi::BindingSetDesc setDesc = CreateDescriptorRangeDesc(
				commandList,
				compute.resources, compute.resourceNum,
				pipeline.compute.descriptorRanges, pipeline.compute.descriptorRangeNum);
			nvrhi::IComputePipeline* csPipeline = nvrhi::checked_cast<nvrhi::IComputePipeline*>(m_pipelines[compute.pipelineIndex].Get());
			nvrhi::BindingSetHandle bindingSet = m_bindingCache->GetOrCreateBindingSet(setDesc, csPipeline->getDesc().bindingLayouts[0].Get());

			uint32_t indirectArgOffset = 0;
			nvrhi::BufferHandle indArg = GetBufferResource(params, output, compute.indirectArg, indirectArgOffset);

			commandList->setBufferState(indArg, nvrhi::ResourceStates::IndirectArgument);
			commandList->commitBarriers();

			nvrhi::ComputeState state;
			state.pipeline = csPipeline;
			state.bindings = { bindingSet };
			state.indirectParams = indArg;

			commandList->setComputeState(state);
			SetPushConstants(commandList, compute.localConstantBufferData, compute.localConstantBufferDataSize);

			commandList->dispatchIndirect(indirectArgOffset + (uint32_t)compute.indirectArgByteOffset);
		}
		break;
		case omm::Gpu::DispatchType::DrawIndexedIndirect:
		{
			const omm::Gpu::DrawIndexedIndirectDesc& draw = desc.drawIndexedIndirect;
			const omm::Gpu::PipelineDesc& pipeline = pipelineDesc->pipelines[draw.pipelineIndex];
			
			assert(pipeline.type == omm::Gpu::PipelineType::Graphics);
			
			nvrhi::BindingSetDesc setDesc = CreateDescriptorRangeDesc(
				commandList,
				draw.resources, draw.resourceNum,
				pipeline.graphics.descriptorRanges, pipeline.graphics.descriptorRangeNum);
			nvrhi::IGraphicsPipeline* gfxPipeline = nvrhi::checked_cast<nvrhi::IGraphicsPipeline*>(m_pipelines[draw.pipelineIndex].Get());
			nvrhi::BindingSetHandle bindingSet = m_bindingCache->GetOrCreateBindingSet(setDesc, gfxPipeline->getDesc().bindingLayouts[0].Get());
			
			uint32_t indirectArgOffset = 0;
			nvrhi::BufferHandle indArg = GetBufferResource(params, output, draw.indirectArg, indirectArgOffset);

			commandList->setBufferState(indArg, nvrhi::ResourceStates::IndirectArgument);
			// commandList->commitBarriers(); UGH. This is done in setGraphicsState.
			
			if (rtv)
				commandList->setTextureState(rtv, nvrhi::AllSubresources, nvrhi::ResourceStates::RenderTarget);

			nvrhi::Viewport viewport;
			viewport.minX = draw.viewport.minWidth;
			viewport.maxX = draw.viewport.maxWidth;
			viewport.minY = draw.viewport.minHeight;
			viewport.maxY = draw.viewport.maxHeight;

			nvrhi::GraphicsState state;
			state.addBindingSet(bindingSet);
			state.setPipeline(gfxPipeline);
			state.setFramebuffer(m_nullFbo);
			state.viewport.addViewportAndScissorRect(viewport);
			state.setIndirectParams(indArg);

			uint32_t indexBufferOffset = 0;
			state.setIndexBuffer({ 
				GetBufferResource(params, output, draw.indexBuffer, indexBufferOffset),
				nvrhi::Format::R32_UINT,
				indexBufferOffset + draw.indexBufferOffset
			});

			uint32_t vertexBufferOffset = 0;
			state.addVertexBuffer({ 
				GetBufferResource(params, output, draw.vertexBuffer, vertexBufferOffset),
				0u, 
				vertexBufferOffset + draw.vertexBufferOffset });
			
			commandList->setGraphicsState(state);
			SetPushConstants(commandList, draw.localConstantBufferData, draw.localConstantBufferDataSize);

			commandList->drawIndexedIndirect((uint32_t)draw.indirectArgByteOffset);
		}
		break;
		default:
			break;
		}
	}

	if (rtv)
		commandList->setTextureState(rtv, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
	commandList->setBufferState(m_globalCBuffer, nvrhi::ResourceStates::ConstantBuffer);
	commandList->setBufferState(output.ommArrayBuffer, nvrhi::ResourceStates::Common);
	commandList->setBufferState(output.ommDescBuffer, nvrhi::ResourceStates::Common);
	commandList->setBufferState(output.ommIndexBuffer, nvrhi::ResourceStates::Common);
	commandList->setBufferState(output.ommDescArrayHistogramBuffer, nvrhi::ResourceStates::Common);
	commandList->setBufferState(output.ommIndexHistogramBuffer, nvrhi::ResourceStates::Common);
	for (auto it : m_transientPool)
		commandList->setBufferState(it, nvrhi::ResourceStates::Common);
	commandList->commitBarriers();
}

void NVRHIVmBakeIntegration::DumpDebug(
	const char* folderName,
	const char* debugName,
	const Input& params,
	const std::vector<uint8_t>& ommArrayBuffer,
	const std::vector<uint8_t>& ommDescBuffer,
	const std::vector<uint8_t>& ommIndexBuffer,
	nvrhi::Format indexBufferFormat,
	const std::vector<uint8_t>& ommDescArrayHistogramBuffer,
	const std::vector<uint8_t>& ommIndexHistogramBuffer,
	const void* indexBuffer,
	const uint32_t indexCount,
	const void* texCoords,
	const float* imageData,
	const uint32_t width,
	const uint32_t height
)
{
	const omm::IndexFormat ommIndexBufferFormat = indexBufferFormat == nvrhi::Format::R32_UINT ? omm::IndexFormat::I32_UINT : omm::IndexFormat::I16_UINT;

	omm::Cpu::BakeResultDesc result;
	result.ommArrayData = ommArrayBuffer.data();
	result.ommArrayDataSize = (uint32_t)ommArrayBuffer.size();
	result.ommDescArray = (const omm::Cpu::OpacityMicromapDesc*)ommDescBuffer.data();
	result.ommDescArrayCount = (uint32_t)(ommDescBuffer.size() / sizeof(omm::Cpu::OpacityMicromapDesc));
	result.ommIndexBuffer = ommIndexBuffer.data();
	result.ommIndexFormat = ommIndexBufferFormat;
	result.ommDescArrayHistogramCount = (uint32_t)(ommDescArrayHistogramBuffer.size() / sizeof(omm::Cpu::OpacityMicromapUsageCount));
	result.ommDescArrayHistogram = (const omm::Cpu::OpacityMicromapUsageCount*)ommDescArrayHistogramBuffer.data();
	result.ommIndexHistogramCount = (uint32_t)(ommIndexHistogramBuffer.size() / sizeof(omm::Cpu::OpacityMicromapUsageCount));
	result.ommIndexHistogram = (const omm::Cpu::OpacityMicromapUsageCount*)ommIndexHistogramBuffer.data();

	omm::Cpu::TextureMipDesc mip;
	mip.width = width;
	mip.height = height;
	mip.textureData = imageData;

	omm::Cpu::TextureDesc texDesc;
	texDesc.format = omm::Cpu::TextureFormat::FP32;
	texDesc.mipCount = 1;
	texDesc.mips = &mip;

	omm::Cpu::Texture texHandle;
	omm::Result res = omm::Cpu::CreateTexture(m_cpuBaker, texDesc, &texHandle);
	assert(res == omm::Result::SUCCESS);

	omm::Cpu::BakeInputDesc config;
	config.alphaMode = /*task.geometry->material->domain == MaterialDomain::AlphaBlended ? omm::AlphaMode::Blend : */omm::AlphaMode::Test;
	config.indexBuffer = indexBuffer;
	config.indexCount = indexCount;
	config.indexFormat = omm::IndexFormat::I32_UINT;
	config.texture = texHandle;
	config.texCoords = texCoords;
	config.texCoordFormat = omm::TexCoordFormat::UV32_FLOAT;
	config.alphaCutoff = params.alphaCutoff;
	config.runtimeSamplerDesc.addressingMode	= GetTextureAddressMode(params.sampleMode);
	config.runtimeSamplerDesc.filter = params.bilinearFilter ? omm::TextureFilterMode::Linear : omm::TextureFilterMode::Nearest;

	res = omm::Debug::SaveAsImages(
		m_baker, config, &result, 
		{
			.path = folderName,
			.filePostfix = debugName,
			.detailedCutout = false, 
			.dumpOnlyFirstOMM = false,
			.monochromeUnknowns = false,
			.oneFile = false 
		}
	);
	assert(res == omm::Result::SUCCESS);

	res = omm::Cpu::DestroyTexture(m_cpuBaker, texHandle);
	assert(res == omm::Result::SUCCESS);
}

omm::Debug::Stats NVRHIVmBakeIntegration::GetStats(const omm::Cpu::BakeResultDesc& desc)
{
	omm::Debug::Stats stats;
	omm::Result statsRes = omm::Debug::GetStats(m_baker, &desc, &stats);
	assert(statsRes == omm::Result::SUCCESS);
	return stats;
}