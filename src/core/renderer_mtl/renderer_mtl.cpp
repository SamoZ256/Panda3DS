#include "PICA/gpu.hpp"
#include "renderer_mtl/renderer_mtl.hpp"
#include "renderer_mtl/objc_helper.hpp"

#include <cmrc/cmrc.hpp>
#include <cstddef>

#include "SDL_metal.h"

using namespace PICA;

CMRC_DECLARE(RendererMTL);

const u16 LIGHT_LUT_TEXTURE_WIDTH = 256;

// HACK: redefinition...
PICA::ColorFmt ToColorFormat(u32 format) {
	switch (format) {
		case 2: return PICA::ColorFmt::RGB565;
		case 3: return PICA::ColorFmt::RGBA5551;
		default: return static_cast<PICA::ColorFmt>(format);
	}
}

MTL::Library* loadLibrary(MTL::Device* device, const cmrc::file& shaderSource) {
	//MTL::CompileOptions* compileOptions = MTL::CompileOptions::alloc()->init();
	NS::Error* error = nullptr;
	MTL::Library* library = device->newLibrary(Metal::createDispatchData(shaderSource.begin(), shaderSource.size()), &error);
	//MTL::Library* library = device->newLibrary(NS::String::string(source.c_str(), NS::ASCIIStringEncoding), compileOptions, &error);
	if (error) {
		Helpers::panic("Error loading shaders: %s", error->description()->cString(NS::ASCIIStringEncoding));
	}

	return library;
}

RendererMTL::RendererMTL(GPU& gpu, const std::array<u32, regNum>& internalRegs, const std::array<u32, extRegNum>& externalRegs)
	: Renderer(gpu, internalRegs, externalRegs) {}
RendererMTL::~RendererMTL() {}

void RendererMTL::reset() {
	colorRenderTargetCache.reset();
	depthStencilRenderTargetCache.reset();
	textureCache.reset();

	// TODO: implement
	Helpers::warn("RendererMTL::reset not implemented");
}

void RendererMTL::display() {
	CA::MetalDrawable* drawable = metalLayer->nextDrawable();
	if (!drawable) {
        return;
	}

	MTL::RenderPassDescriptor* renderPassDescriptor = MTL::RenderPassDescriptor::alloc()->init();
	MTL::RenderPassColorAttachmentDescriptor* colorAttachment = renderPassDescriptor->colorAttachments()->object(0);
	colorAttachment->setTexture(drawable->texture());
	colorAttachment->setLoadAction(MTL::LoadActionClear);
	colorAttachment->setClearColor(MTL::ClearColor{0.0f, 0.0f, 0.0f, 1.0f});
	colorAttachment->setStoreAction(MTL::StoreActionStore);

	beginRenderPassIfNeeded(renderPassDescriptor, false, drawable->texture());
	renderCommandEncoder->setRenderPipelineState(displayPipeline);
	renderCommandEncoder->setFragmentSamplerState(nearestSampler, 0);

	using namespace PICA::ExternalRegs;

	// Top screen
	{
		const u32 topActiveFb = externalRegs[Framebuffer0Select] & 1;
		const u32 topScreenAddr = externalRegs[topActiveFb == 0 ? Framebuffer0AFirstAddr : Framebuffer0ASecondAddr];
		auto topScreen = colorRenderTargetCache.findFromAddress(topScreenAddr);

		if (topScreen) {
		    clearColor(nullptr, topScreen->get().texture);
			renderCommandEncoder->setViewport(MTL::Viewport{0, 0, 400, 240, 0.0f, 1.0f});
			renderCommandEncoder->setFragmentTexture(topScreen->get().texture, 0);
			renderCommandEncoder->drawPrimitives(MTL::PrimitiveTypeTriangleStrip, NS::UInteger(0), NS::UInteger(4));
		}
	}

	// Bottom screen
	{
		const u32 bottomActiveFb = externalRegs[Framebuffer1Select] & 1;
		const u32 bottomScreenAddr = externalRegs[bottomActiveFb == 0 ? Framebuffer1AFirstAddr : Framebuffer1ASecondAddr];
		auto bottomScreen = colorRenderTargetCache.findFromAddress(bottomScreenAddr);

		if (bottomScreen) {
            clearColor(nullptr, bottomScreen->get().texture);
			renderCommandEncoder->setViewport(MTL::Viewport{40, 240, 320, 240, 0.0f, 1.0f});
			renderCommandEncoder->setFragmentTexture(bottomScreen->get().texture, 0);
			renderCommandEncoder->drawPrimitives(MTL::PrimitiveTypeTriangleStrip, NS::UInteger(0), NS::UInteger(4));
		}
	}

	endRenderPass();

	for (u32 i = 0; i < commandBuffers.size(); i++) {
	    // Present the drawable on the last command buffer
    	if (i == commandBuffers.size() - 1) {
    	    commandBuffers[i]->commandBuffer->presentDrawable(drawable);
    	}
		commandBuffers[i]->commandBuffer->commit();
	}
	commandBuffers.clear();

	// Inform the vertex buffer cache that the frame ended
	vertexBufferCache.endFrame();
}

void RendererMTL::initGraphicsContext(SDL_Window* window) {
	// TODO: what should be the type of the view?
	void* view = SDL_Metal_CreateView(window);
	metalLayer = (CA::MetalLayer*)SDL_Metal_GetLayer(view);
	device = MTL::CreateSystemDefaultDevice();
	metalLayer->setDevice(device);
	commandQueue = device->newCommandQueue();

	// -------- Objects --------

	// Textures
	MTL::TextureDescriptor* textureDescriptor = MTL::TextureDescriptor::alloc()->init();
	textureDescriptor->setTextureType(MTL::TextureType1DArray);
	textureDescriptor->setPixelFormat(MTL::PixelFormatR16Uint);
	textureDescriptor->setWidth(LIGHT_LUT_TEXTURE_WIDTH);
	textureDescriptor->setArrayLength(Lights::LUT_Count);
	textureDescriptor->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
	textureDescriptor->setStorageMode(MTL::StorageModePrivate);

	lightLUTTextureArray = device->newTexture(textureDescriptor);
	textureDescriptor->release();

	// Samplers
	MTL::SamplerDescriptor* samplerDescriptor = MTL::SamplerDescriptor::alloc()->init();
	nearestSampler = device->newSamplerState(samplerDescriptor);

	samplerDescriptor->setMinFilter(MTL::SamplerMinMagFilterLinear);
	samplerDescriptor->setMagFilter(MTL::SamplerMinMagFilterLinear);
	linearSampler = device->newSamplerState(samplerDescriptor);

	samplerDescriptor->release();

	// -------- Pipelines --------

	// Load shaders
	auto mtlResources = cmrc::RendererMTL::get_filesystem();
	MTL::Library* library = loadLibrary(device, mtlResources.open("metal_shaders.metallib"));
	MTL::Library* copyToLutTextureLibrary = loadLibrary(device, mtlResources.open("metal_copy_to_lut_texture.metallib"));

	// Display
	MTL::Function* vertexDisplayFunction = library->newFunction(NS::String::string("vertexDisplay", NS::ASCIIStringEncoding));
	MTL::Function* fragmentDisplayFunction = library->newFunction(NS::String::string("fragmentDisplay", NS::ASCIIStringEncoding));

	MTL::RenderPipelineDescriptor* displayPipelineDescriptor = MTL::RenderPipelineDescriptor::alloc()->init();
	displayPipelineDescriptor->setVertexFunction(vertexDisplayFunction);
	displayPipelineDescriptor->setFragmentFunction(fragmentDisplayFunction);
	auto* displayColorAttachment = displayPipelineDescriptor->colorAttachments()->object(0);
	displayColorAttachment->setPixelFormat(MTL::PixelFormat::PixelFormatBGRA8Unorm);

	NS::Error* error = nullptr;
	displayPipeline = device->newRenderPipelineState(displayPipelineDescriptor, &error);
	if (error) {
		Helpers::panic("Error creating display pipeline state: %s", error->description()->cString(NS::ASCIIStringEncoding));
	}

	// Blit
	MTL::Function* vertexBlitFunction = library->newFunction(NS::String::string("vertexBlit", NS::ASCIIStringEncoding));
	MTL::Function* fragmentBlitFunction = library->newFunction(NS::String::string("fragmentBlit", NS::ASCIIStringEncoding));

	blitPipelineCache.set(device, vertexBlitFunction, fragmentBlitFunction);

	// Draw
	MTL::Function* vertexDrawFunction = library->newFunction(NS::String::string("vertexDraw", NS::ASCIIStringEncoding));

	// -------- Vertex descriptor --------
	MTL::VertexDescriptor* vertexDescriptor = MTL::VertexDescriptor::alloc()->init();

	// Position
	MTL::VertexAttributeDescriptor* positionAttribute = vertexDescriptor->attributes()->object(0);
	positionAttribute->setFormat(MTL::VertexFormatFloat4);
	positionAttribute->setOffset(offsetof(Vertex, s.positions));
	positionAttribute->setBufferIndex(VERTEX_BUFFER_BINDING_INDEX);

	// Quaternion
	MTL::VertexAttributeDescriptor* quaternionAttribute = vertexDescriptor->attributes()->object(1);
	quaternionAttribute->setFormat(MTL::VertexFormatFloat4);
	quaternionAttribute->setOffset(offsetof(Vertex, s.quaternion));
	quaternionAttribute->setBufferIndex(VERTEX_BUFFER_BINDING_INDEX);

	// Color
	MTL::VertexAttributeDescriptor* colorAttribute = vertexDescriptor->attributes()->object(2);
	colorAttribute->setFormat(MTL::VertexFormatFloat4);
	colorAttribute->setOffset(offsetof(Vertex, s.colour));
	colorAttribute->setBufferIndex(VERTEX_BUFFER_BINDING_INDEX);

	// Texture coordinate 0
	MTL::VertexAttributeDescriptor* texCoord0Attribute = vertexDescriptor->attributes()->object(3);
	texCoord0Attribute->setFormat(MTL::VertexFormatFloat2);
	texCoord0Attribute->setOffset(offsetof(Vertex, s.texcoord0));
	texCoord0Attribute->setBufferIndex(VERTEX_BUFFER_BINDING_INDEX);

	// Texture coordinate 1
	MTL::VertexAttributeDescriptor* texCoord1Attribute = vertexDescriptor->attributes()->object(4);
	texCoord1Attribute->setFormat(MTL::VertexFormatFloat2);
	texCoord1Attribute->setOffset(offsetof(Vertex, s.texcoord1));
	texCoord1Attribute->setBufferIndex(VERTEX_BUFFER_BINDING_INDEX);

	// Texture coordinate 0 W
	MTL::VertexAttributeDescriptor* texCoord0WAttribute = vertexDescriptor->attributes()->object(5);
	texCoord0WAttribute->setFormat(MTL::VertexFormatFloat);
	texCoord0WAttribute->setOffset(offsetof(Vertex, s.texcoord0_w));
	texCoord0WAttribute->setBufferIndex(VERTEX_BUFFER_BINDING_INDEX);

	// View
	MTL::VertexAttributeDescriptor* viewAttribute = vertexDescriptor->attributes()->object(6);
	viewAttribute->setFormat(MTL::VertexFormatFloat3);
	viewAttribute->setOffset(offsetof(Vertex, s.view));
	viewAttribute->setBufferIndex(VERTEX_BUFFER_BINDING_INDEX);

	// Texture coordinate 2
	MTL::VertexAttributeDescriptor* texCoord2Attribute = vertexDescriptor->attributes()->object(7);
	texCoord2Attribute->setFormat(MTL::VertexFormatFloat2);
	texCoord2Attribute->setOffset(offsetof(Vertex, s.texcoord2));
	texCoord2Attribute->setBufferIndex(VERTEX_BUFFER_BINDING_INDEX);

	MTL::VertexBufferLayoutDescriptor* vertexBufferLayout = vertexDescriptor->layouts()->object(VERTEX_BUFFER_BINDING_INDEX);
	vertexBufferLayout->setStride(sizeof(Vertex));
	vertexBufferLayout->setStepFunction(MTL::VertexStepFunctionPerVertex);
	vertexBufferLayout->setStepRate(1);

	drawPipelineCache.set(device, library, vertexDrawFunction, vertexDescriptor);

	// Copy to LUT texture
	MTL::FunctionConstantValues* constants = MTL::FunctionConstantValues::alloc()->init();
    constants->setConstantValue(&LIGHT_LUT_TEXTURE_WIDTH, MTL::DataTypeUShort, NS::UInteger(0));

    error = nullptr;
    MTL::Function* vertexCopyToLutTextureFunction = copyToLutTextureLibrary->newFunction(NS::String::string("vertexCopyToLutTexture", NS::ASCIIStringEncoding), constants, &error);
    if (error) {
        Helpers::panic("Error creating copy_to_lut_texture vertex function: %s", error->description()->cString(NS::ASCIIStringEncoding));
    }
    constants->release();

	MTL::RenderPipelineDescriptor* copyToLutTexturePipelineDescriptor = MTL::RenderPipelineDescriptor::alloc()->init();
	copyToLutTexturePipelineDescriptor->setVertexFunction(vertexCopyToLutTextureFunction);
	// Disable rasterization
	copyToLutTexturePipelineDescriptor->setRasterizationEnabled(false);

	error = nullptr;
	copyToLutTexturePipeline = device->newRenderPipelineState(copyToLutTexturePipelineDescriptor, &error);
	if (error) {
		Helpers::panic("Error creating copy_to_lut_texture pipeline state: %s", error->description()->cString(NS::ASCIIStringEncoding));
	}

	// Depth stencil cache
	depthStencilCache.set(device);

	// Vertex buffer cache
	vertexBufferCache.set(device);

	// -------- Depth stencil state --------
	MTL::DepthStencilDescriptor* depthStencilDescriptor = MTL::DepthStencilDescriptor::alloc()->init();
	defaultDepthStencilState = device->newDepthStencilState(depthStencilDescriptor);
}

void RendererMTL::clearBuffer(u32 startAddress, u32 endAddress, u32 value, u32 control) {
	const auto color = colorRenderTargetCache.findFromAddress(startAddress);
	if (color) {
		const float r = Helpers::getBits<24, 8>(value) / 255.0f;
		const float g = Helpers::getBits<16, 8>(value) / 255.0f;
		const float b = Helpers::getBits<8, 8>(value) / 255.0f;
		const float a = (value & 0xff) / 255.0f;

		colorClearOps.push_back({color->get().texture, r, g, b, a});

		return;
	}

	const auto depth = depthStencilRenderTargetCache.findFromAddress(startAddress);
	if (depth) {
		float depthVal;
		const auto format = depth->get().format;
		if (format == DepthFmt::Depth16) {
			depthVal = (value & 0xffff) / 65535.0f;
		} else {
			depthVal = (value & 0xffffff) / 16777215.0f;
		}

		depthClearOps.push_back({depth->get().texture, depthVal});

		if (format == DepthFmt::Depth24Stencil8) {
            const u8 stencilVal = value >> 24;
            stencilClearOps.push_back({depth->get().texture, stencilVal});
		}

		return;
	}

	Helpers::warn("[RendererMTL::ClearBuffer] No buffer found!\n");
}

void RendererMTL::displayTransfer(u32 inputAddr, u32 outputAddr, u32 inputSize, u32 outputSize, u32 flags) {
	const u32 inputWidth = inputSize & 0xffff;
	const u32 inputHeight = inputSize >> 16;
	const auto inputFormat = ToColorFormat(Helpers::getBits<8, 3>(flags));
	const auto outputFormat = ToColorFormat(Helpers::getBits<12, 3>(flags));
	const bool verticalFlip = flags & 1;
	const PICA::Scaling scaling = static_cast<PICA::Scaling>(Helpers::getBits<24, 2>(flags));

	u32 outputWidth = outputSize & 0xffff;
	u32 outputHeight = outputSize >> 16;

	auto srcFramebuffer = getColorRenderTarget(inputAddr, inputFormat, inputWidth, outputHeight);
	clearColor(nullptr, srcFramebuffer->texture);
	Math::Rect<u32> srcRect = srcFramebuffer->getSubRect(inputAddr, outputWidth, outputHeight);

	if (verticalFlip) {
		std::swap(srcRect.bottom, srcRect.top);
	}

	// Apply scaling for the destination rectangle.
	if (scaling == PICA::Scaling::X || scaling == PICA::Scaling::XY) {
		outputWidth >>= 1;
	}

	if (scaling == PICA::Scaling::XY) {
		outputHeight >>= 1;
	}

	auto destFramebuffer = getColorRenderTarget(outputAddr, outputFormat, outputWidth, outputHeight);
	Math::Rect<u32> destRect = destFramebuffer->getSubRect(outputAddr, outputWidth, outputHeight);

	if (inputWidth != outputWidth) {
		// Helpers::warn("Strided display transfer is not handled correctly!\n");
	}

	// TODO: respect regions
	MTL::RenderPassDescriptor* renderPassDescriptor = MTL::RenderPassDescriptor::alloc()->init();
	MTL::RenderPassColorAttachmentDescriptor* colorAttachment = renderPassDescriptor->colorAttachments()->object(0);
	colorAttachment->setTexture(destFramebuffer->texture);
	colorAttachment->setLoadAction(MTL::LoadActionClear);
	colorAttachment->setClearColor(MTL::ClearColor{0.0, 0.0, 0.0, 1.0});
	colorAttachment->setStoreAction(MTL::StoreActionStore);

	// Pipeline
	Metal::BlitPipelineHash hash{destFramebuffer->format, DepthFmt::Unknown1};
	auto blitPipeline = blitPipelineCache.get(hash);

	beginRenderPassIfNeeded(renderPassDescriptor, false, destFramebuffer->texture);
	renderCommandEncoder->setRenderPipelineState(blitPipeline);
	renderCommandEncoder->setFragmentTexture(srcFramebuffer->texture, 0);
	renderCommandEncoder->setFragmentSamplerState(nearestSampler, 0);

	renderCommandEncoder->drawPrimitives(MTL::PrimitiveTypeTriangleStrip, NS::UInteger(0), NS::UInteger(4));
}

void RendererMTL::textureCopy(u32 inputAddr, u32 outputAddr, u32 totalBytes, u32 inputSize, u32 outputSize, u32 flags) {
	// TODO: implement
	Helpers::warn("RendererMTL::textureCopy not implemented");
}

void RendererMTL::drawVertices(PICA::PrimType primType, std::span<const PICA::Vertex> vertices) {
	// Color
	auto colorRenderTarget = getColorRenderTarget(colourBufferLoc, colourBufferFormat, fbSize[0], fbSize[1]);

	// Depth stencil
	const u32 depthControl = regs[PICA::InternalRegs::DepthAndColorMask];
	const bool depthStencilWrite = regs[PICA::InternalRegs::DepthBufferWrite];
	const bool depthEnable = depthControl & 0x1;
	const bool depthWriteEnable = Helpers::getBit<12>(depthControl);
	const u8 depthFunc = Helpers::getBits<4, 3>(depthControl);
	const u8 colorMask = Helpers::getBits<8, 4>(depthControl);
	// TODO: color mask
	// gl.setColourMask(colorMask & 0x1, colorMask & 0x2, colorMask & 0x4, colorMask & 0x8);

	Metal::DepthStencilHash depthStencilHash{false, 1};
	depthStencilHash.stencilConfig = regs[PICA::InternalRegs::StencilTest];
	depthStencilHash.stencilOpConfig = regs[PICA::InternalRegs::StencilOp];
	const bool stencilEnable = Helpers::getBit<0>(depthStencilHash.stencilConfig);

	std::optional<Metal::DepthStencilRenderTarget> depthStencilRenderTarget = std::nullopt;
	if (depthEnable) {
		depthStencilHash.depthStencilWrite = depthWriteEnable && depthStencilWrite;
		depthStencilHash.depthFunc = depthFunc;
		depthStencilRenderTarget = getDepthRenderTarget();
	} else {
		if (depthWriteEnable) {
			depthStencilHash.depthStencilWrite = true;
			depthStencilRenderTarget = getDepthRenderTarget();
		} else if (stencilEnable) {
			depthStencilRenderTarget = getDepthRenderTarget();
		}
	}

	// Depth uniforms
	struct {
        float depthScale;
       	float depthOffset;
       	bool depthMapEnable;
	} depthUniforms;
	depthUniforms.depthScale = Floats::f24::fromRaw(regs[PICA::InternalRegs::DepthScale] & 0xffffff).toFloat32();
   	depthUniforms.depthOffset = Floats::f24::fromRaw(regs[PICA::InternalRegs::DepthOffset] & 0xffffff).toFloat32();
   	depthUniforms.depthMapEnable = regs[PICA::InternalRegs::DepthmapEnable] & 1;

	// -------- Pipeline --------
	Metal::DrawPipelineHash pipelineHash{colorRenderTarget->format, DepthFmt::Unknown1};
	if (depthStencilRenderTarget) {
        pipelineHash.depthFmt = depthStencilRenderTarget->format;
    }
    pipelineHash.lightingEnabled = regs[0x008F] & 1;
    pipelineHash.lightingNumLights = regs[0x01C2] & 0x7;
    pipelineHash.lightingConfig1 = regs[0x01C4u] >> 16; // Last 16 bits are unused, so skip them
    pipelineHash.alphaControl = regs[0x104];

	// Blending and logic op
	pipelineHash.blendEnabled = (regs[PICA::InternalRegs::ColourOperation] & (1 << 8)) != 0;

	u8 logicOp = 3; // Copy, which doesn't do anything
	if (pipelineHash.blendEnabled) {
    	pipelineHash.blendControl = regs[PICA::InternalRegs::BlendFunc];
        // TODO: constant color
       	//pipelineHash.constantColor = regs[PICA::InternalRegs::BlendColour];
    	//const u8 r = pipelineHash.constantColor & 0xff;
    	//const u8 g = Helpers::getBits<8, 8>(pipelineHash.constantColor);
    	//const u8 b = Helpers::getBits<16, 8>(pipelineHash.constantColor);
    	//const u8 a = Helpers::getBits<24, 8>(pipelineHash.constantColor);
	} else {
	    logicOp = Helpers::getBits<0, 4>(regs[PICA::InternalRegs::LogicOp]);
	}

	MTL::RenderPipelineState* pipeline = drawPipelineCache.get(pipelineHash);

	// Depth stencil state
	MTL::DepthStencilState* depthStencilState = depthStencilCache.get(depthStencilHash);

	// -------- Render --------
	MTL::RenderPassDescriptor* renderPassDescriptor = MTL::RenderPassDescriptor::alloc()->init();
	bool doesClear = clearColor(renderPassDescriptor, colorRenderTarget->texture);
    if (depthStencilRenderTarget) {
        if (clearDepth(renderPassDescriptor, depthStencilRenderTarget->texture))
            doesClear = true;
        if (depthStencilRenderTarget->format == DepthFmt::Depth24Stencil8) {
            if (clearStencil(renderPassDescriptor, depthStencilRenderTarget->texture))
                doesClear = true;
        }
    }

	beginRenderPassIfNeeded(renderPassDescriptor, doesClear, colorRenderTarget->texture, (depthStencilRenderTarget ? depthStencilRenderTarget->texture : nullptr));

	// Update the LUT texture if necessary
	if (gpu.lightingLUTDirty) {
		updateLightingLUT(renderCommandEncoder);
	}

	renderCommandEncoder->setRenderPipelineState(pipeline);
	renderCommandEncoder->setDepthStencilState(depthStencilState);
	// If size is < 4KB, use inline vertex data, otherwise use a buffer
	if (vertices.size_bytes() < 4 * 1024) {
		renderCommandEncoder->setVertexBytes(vertices.data(), vertices.size_bytes(), VERTEX_BUFFER_BINDING_INDEX);
	} else {
	    Metal::BufferHandle buffer = vertexBufferCache.get(vertices);
		renderCommandEncoder->setVertexBuffer(buffer.buffer, buffer.offset, VERTEX_BUFFER_BINDING_INDEX);
	}

	// Bind resources
	setupTextureEnvState(renderCommandEncoder);
	bindTexturesToSlots(renderCommandEncoder);
	renderCommandEncoder->setVertexBytes(&regs[0x48], (0x200 - 0x48) * sizeof(regs[0]), 0);
	renderCommandEncoder->setFragmentBytes(&regs[0x48], (0x200 - 0x48) * sizeof(regs[0]), 0);
	renderCommandEncoder->setVertexBytes(&depthUniforms, sizeof(depthUniforms), 2);
	renderCommandEncoder->setFragmentBytes(&logicOp, sizeof(logicOp), 2);

	renderCommandEncoder->drawPrimitives(toMTLPrimitiveType(primType), NS::UInteger(0), NS::UInteger(vertices.size()));
}

void RendererMTL::screenshot(const std::string& name) {
	// TODO: implement
	Helpers::warn("RendererMTL::screenshot not implemented");
}

void RendererMTL::deinitGraphicsContext() {
	colorRenderTargetCache.reset();
	depthStencilRenderTargetCache.reset();
	textureCache.reset();

	// TODO: implement
	Helpers::warn("RendererMTL::deinitGraphicsContext not implemented");
}

std::optional<Metal::ColorRenderTarget> RendererMTL::getColorRenderTarget(
	u32 addr, PICA::ColorFmt format, u32 width, u32 height, bool createIfnotFound
) {
	// Try to find an already existing buffer that contains the provided address
	// This is a more relaxed check compared to getColourFBO as display transfer/texcopy may refer to
	// subrect of a surface and in case of texcopy we don't know the format of the surface.
	auto buffer = colorRenderTargetCache.findFromAddress(addr);
	if (buffer.has_value()) {
		return buffer.value().get();
	}

	if (!createIfnotFound) {
		return std::nullopt;
	}

	// Otherwise create and cache a new buffer.
	Metal::ColorRenderTarget sampleBuffer(device, addr, format, width, height);

	return colorRenderTargetCache.add(sampleBuffer);
}

Metal::DepthStencilRenderTarget& RendererMTL::getDepthRenderTarget() {
	Metal::DepthStencilRenderTarget sampleBuffer(device, depthBufferLoc, depthBufferFormat, fbSize[0], fbSize[1]);
	auto buffer = depthStencilRenderTargetCache.find(sampleBuffer);

	if (buffer.has_value()) {
		return buffer.value().get();
	} else {
		return depthStencilRenderTargetCache.add(sampleBuffer);
	}
}

Metal::Texture& RendererMTL::getTexture(Metal::Texture& tex) {
	auto buffer = textureCache.find(tex);

	if (buffer.has_value()) {
		return buffer.value().get();
	} else {
		const auto textureData = std::span{gpu.getPointerPhys<u8>(tex.location), tex.sizeInBytes()};  // Get pointer to the texture data in 3DS memory
		Metal::Texture& newTex = textureCache.add(tex);
		newTex.decodeTexture(textureData);

		return newTex;
	}
}

void RendererMTL::setupTextureEnvState(MTL::RenderCommandEncoder* encoder) {
	static constexpr std::array<u32, 6> ioBases = {
		PICA::InternalRegs::TexEnv0Source, PICA::InternalRegs::TexEnv1Source, PICA::InternalRegs::TexEnv2Source,
		PICA::InternalRegs::TexEnv3Source, PICA::InternalRegs::TexEnv4Source, PICA::InternalRegs::TexEnv5Source,
	};

	struct {
		u32 textureEnvSourceRegs[6];
		u32 textureEnvOperandRegs[6];
		u32 textureEnvCombinerRegs[6];
		u32 textureEnvScaleRegs[6];
	} envState;
	u32 textureEnvColourRegs[6];

	for (int i = 0; i < 6; i++) {
		const u32 ioBase = ioBases[i];

		envState.textureEnvSourceRegs[i] = regs[ioBase];
		envState.textureEnvOperandRegs[i] = regs[ioBase + 1];
		envState.textureEnvCombinerRegs[i] = regs[ioBase + 2];
		textureEnvColourRegs[i] = regs[ioBase + 3];
		envState.textureEnvScaleRegs[i] = regs[ioBase + 4];
	}

	encoder->setVertexBytes(&textureEnvColourRegs, sizeof(textureEnvColourRegs), 1);
	encoder->setFragmentBytes(&envState, sizeof(envState), 1);
}

void RendererMTL::bindTexturesToSlots(MTL::RenderCommandEncoder* encoder) {
	static constexpr std::array<u32, 3> ioBases = {
		PICA::InternalRegs::Tex0BorderColor,
		PICA::InternalRegs::Tex1BorderColor,
		PICA::InternalRegs::Tex2BorderColor,
	};

	for (int i = 0; i < 3; i++) {
		if ((regs[PICA::InternalRegs::TexUnitCfg] & (1 << i)) == 0) {
			continue;
		}

		const size_t ioBase = ioBases[i];

		const u32 dim = regs[ioBase + 1];
		const u32 config = regs[ioBase + 2];
		const u32 height = dim & 0x7ff;
		const u32 width = Helpers::getBits<16, 11>(dim);
		const u32 addr = (regs[ioBase + 4] & 0x0FFFFFFF) << 3;
		u32 format = regs[ioBase + (i == 0 ? 13 : 5)] & 0xF;

		if (addr != 0) [[likely]] {
			Metal::Texture targetTex(device, addr, static_cast<PICA::TextureFmt>(format), width, height, config);
			auto tex = getTexture(targetTex);
			encoder->setFragmentTexture(tex.texture, i);
			encoder->setFragmentSamplerState(tex.sampler ? tex.sampler : nearestSampler, i);
		} else {
			// TODO: bind a dummy texture?
		}
	}

	// LUT texture
	encoder->setFragmentTexture(lightLUTTextureArray, 3);
	encoder->setFragmentSamplerState(linearSampler, 3);
}

void RendererMTL::updateLightingLUT(MTL::RenderCommandEncoder* encoder) {
	gpu.lightingLUTDirty = false;
	std::array<u16, GPU::LightingLutSize> u16_lightinglut;

	for (int i = 0; i < gpu.lightingLUT.size(); i++) {
		uint64_t value = gpu.lightingLUT[i] & ((1 << 12) - 1);
		u16_lightinglut[i] = value * 65535 / 4095;
	}

	//for (int i = 0; i < Lights::LUT_Count; i++) {
	//    lightLUTTextureArray->replaceRegion(MTL::Region(0, 0, LIGHT_LUT_TEXTURE_WIDTH, 1), 0, i, u16_lightinglut.data() + LIGHT_LUT_TEXTURE_WIDTH * i, 0, 0);
	//}

	renderCommandEncoder->setRenderPipelineState(copyToLutTexturePipeline);
	renderCommandEncoder->setDepthStencilState(defaultDepthStencilState);
	renderCommandEncoder->setVertexTexture(lightLUTTextureArray, 0);
	renderCommandEncoder->setVertexBytes(u16_lightinglut.data(), sizeof(u16_lightinglut), 0);

	renderCommandEncoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), GPU::LightingLutSize);
}
