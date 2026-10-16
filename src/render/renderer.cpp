#include "renderer.hpp"

#include "imgui_impl_vulkan.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace {
const uint32_t kDishVert[] =
#include "dish.vert.inc"
    ;
const uint32_t kDishFrag[] =
#include "dish.frag.inc"
    ;
const uint32_t kCellVert[] =
#include "cell.vert.inc"
    ;
const uint32_t kCellFrag[] =
#include "cell.frag.inc"
    ;
const uint32_t kLineVert[] =
#include "line.vert.inc"
    ;
const uint32_t kLineFrag[] =
#include "line.frag.inc"
    ;
const uint32_t kParticleVert[] =
#include "particle.vert.inc"
    ;
const uint32_t kParticleFrag[] =
#include "particle.frag.inc"
    ;

struct PushConstants {
    float cam[4];
    float params[4];
    float extra[4];
    float extra2[4];
};
}  // namespace

bool Renderer::init(VkContext& ctx) {
    ctx_ = &ctx;
    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &pcr;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &li, nullptr, &layout_));

    // The vertex layouts live in the renderer so a pipeline rebuilt from the player's shader keeps the same inputs.
    static const VkVertexInputAttributeDescription kCellAttrs[] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(CellInstance, x)},
        {1, 0, VK_FORMAT_R32_SFLOAT, offsetof(CellInstance, radius)},
        {2, 0, VK_FORMAT_R32_SFLOAT, offsetof(CellInstance, angle)},
        {3, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(CellInstance, color)},
        {4, 0, VK_FORMAT_R32_UINT, offsetof(CellInstance, typeFlags)},
        {5, 0, VK_FORMAT_R32_SFLOAT, offsetof(CellInstance, p0)},
        {6, 0, VK_FORMAT_R32_SFLOAT, offsetof(CellInstance, p1)},
        {7, 0, VK_FORMAT_R32G32B32A32_UINT, offsetof(CellInstance, shape)},
        {8, 0, VK_FORMAT_R32G32B32A32_UINT, offsetof(CellInstance, shape) + 16},
    };
    static const VkVertexInputAttributeDescription kLineAttrs[] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(LineInstance, ax)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(LineInstance, bx)},
        {2, 0, VK_FORMAT_R32_SFLOAT, offsetof(LineInstance, width)},
        {3, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(LineInstance, color)},
        {4, 0, VK_FORMAT_R32_SFLOAT, offsetof(LineInstance, pinch)},
    };
    static const VkVertexInputAttributeDescription kPartAttrs[] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ParticleInstance, x)},
        {1, 0, VK_FORMAT_R32_SFLOAT, offsetof(ParticleInstance, radius)},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(ParticleInstance, color)},
        {3, 0, VK_FORMAT_R32_UINT, offsetof(ParticleInstance, kind)},
        {4, 0, VK_FORMAT_R32_SFLOAT, offsetof(ParticleInstance, angle)},
    };

    dishPipe_ = makePipeline(kDishVert, sizeof(kDishVert), kDishFrag, sizeof(kDishFrag), nullptr, 0, 0);
    cellPipe_ = makePipeline(kCellVert, sizeof(kCellVert), kCellFrag, sizeof(kCellFrag), kCellAttrs, 9, sizeof(CellInstance));
    linePipe_ = makePipeline(kLineVert, sizeof(kLineVert), kLineFrag, sizeof(kLineFrag), kLineAttrs, 5, sizeof(LineInstance));
    particlePipe_ = makePipeline(kParticleVert, sizeof(kParticleVert), kParticleFrag, sizeof(kParticleFrag), kPartAttrs, 5,
                                 sizeof(ParticleInstance));

    // Offscreen pass: same colour format as the swapchain so the pipelines stay compatible.
    VkAttachmentDescription att{};
    att.format = ctx.swapFormat();
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;  // previous frame's sampling must finish before we overwrite
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;  // writes visible to ImGui's fragment shader
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 1;
    rpi.pAttachments = &att;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    rpi.dependencyCount = 2;
    rpi.pDependencies = deps;
    VK_CHECK(vkCreateRenderPass(ctx.device(), &rpi, nullptr, &offscreenPass_));
    return true;
}

void Renderer::destroyTarget(OffscreenTarget& t) {
    VkDevice dev = ctx_->device();
    if (t.texture) ImGui_ImplVulkan_RemoveTexture(t.texture);
    if (t.framebuffer) vkDestroyFramebuffer(dev, t.framebuffer, nullptr);
    if (t.view) vkDestroyImageView(dev, t.view, nullptr);
    if (t.image) vkDestroyImage(dev, t.image, nullptr);
    if (t.memory) vkFreeMemory(dev, t.memory, nullptr);
    if (t.sampler) vkDestroySampler(dev, t.sampler, nullptr);
    t = OffscreenTarget{};
}

OffscreenTarget& Renderer::preview(uint32_t w, uint32_t h) {
    w = std::max(16u, w);
    h = std::max(16u, h);
    OffscreenTarget& t = preview_;
    if (t.image && t.extent.width == w && t.extent.height == h) return t;
    VkDevice dev = ctx_->device();
    if (t.image) {
        vkDeviceWaitIdle(dev);  // the old image may still be sampled by a frame in flight
        destroyTarget(t);
    }
    t.extent = {w, h};
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = ctx_->swapFormat();
    ii.extent = {w, h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(dev, &ii, nullptr, &t.image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, t.image, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = ctx_->findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(dev, &mai, nullptr, &t.memory));
    VK_CHECK(vkBindImageMemory(dev, t.image, t.memory, 0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ii.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(dev, &vi, nullptr, &t.view));
    VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fbi.renderPass = offscreenPass_;
    fbi.attachmentCount = 1;
    fbi.pAttachments = &t.view;
    fbi.width = w;
    fbi.height = h;
    fbi.layers = 1;
    VK_CHECK(vkCreateFramebuffer(dev, &fbi, nullptr, &t.framebuffer));
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 1.0f;
    VK_CHECK(vkCreateSampler(dev, &si, nullptr, &t.sampler));
    t.texture = ImGui_ImplVulkan_AddTexture(t.sampler, t.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return t;
}

void Renderer::beginPreviewPass(const float clear[4]) {
    OffscreenTarget& t = preview_;
    VkClearValue cv{};
    std::memcpy(cv.color.float32, clear, sizeof(float) * 4);
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = offscreenPass_;
    rbi.framebuffer = t.framebuffer;
    rbi.renderArea.extent = t.extent;
    rbi.clearValueCount = 1;
    rbi.pClearValues = &cv;
    VkCommandBuffer cmd = ctx_->cmd();
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0, 0, (float)t.extent.width, (float)t.extent.height, 0, 1};
    VkRect2D sc{{0, 0}, t.extent};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

void Renderer::endPreviewPass() { vkCmdEndRenderPass(ctx_->cmd()); }

VkPipeline Renderer::makePipeline(const uint32_t* vs, size_t vsSize, const uint32_t* fs, size_t fsSize,
                                  const VkVertexInputAttributeDescription* attrs, uint32_t attrCount, uint32_t stride) {
    VkDevice dev = ctx_->device();
    VkShaderModule vm = ctx_->createShader(vs, vsSize);
    VkShaderModule fm = ctx_->createShader(fs, fsSize);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vm, "main"};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fm, "main"};

    VkVertexInputBindingDescription binding{0, stride, VK_VERTEX_INPUT_RATE_INSTANCE};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (attrCount) {
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &binding;
        vi.vertexAttributeDescriptionCount = attrCount;
        vi.pVertexAttributeDescriptions = attrs;
    }
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = attrCount ? VK_TRUE : VK_FALSE;  // dish is opaque, everything else premultiplied alpha
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.stageCount = 2;
    pi.pStages = stages;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &ds;
    pi.layout = layout_;
    pi.renderPass = ctx_->renderPass();
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pi, nullptr, &p));
    vkDestroyShaderModule(dev, vm, nullptr);
    vkDestroyShaderModule(dev, fm, nullptr);
    return p;
}

// A pipeline rebuilt from the player's shader, with the same vertex layout as the original. Unlike makePipeline this
// one does not abort on failure: a bad shader is the player's mistake, not a reason to close the game.
static VkPipeline tryMakePipeline(VkContext& ctx, VkPipelineLayout layout, const uint32_t* vs, size_t vsSize,
                                  const uint32_t* fs, size_t fsSize, const VkVertexInputAttributeDescription* attrs,
                                  uint32_t attrCount, uint32_t stride) {
    VkDevice dev = ctx.device();
    VkShaderModule vm = ctx.createShader(vs, vsSize);
    VkShaderModule fm = ctx.createShader(fs, fsSize);
    if (vm == VK_NULL_HANDLE || fm == VK_NULL_HANDLE) {
        if (vm) vkDestroyShaderModule(dev, vm, nullptr);
        if (fm) vkDestroyShaderModule(dev, fm, nullptr);
        return VK_NULL_HANDLE;
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vm, "main"};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fm, "main"};

    VkVertexInputBindingDescription binding{0, stride, VK_VERTEX_INPUT_RATE_INSTANCE};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (attrCount) {
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &binding;
        vi.vertexAttributeDescriptionCount = attrCount;
        vi.pVertexAttributeDescriptions = attrs;
    }
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = attrCount ? VK_TRUE : VK_FALSE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.stageCount = 2;
    pi.pStages = stages;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &ds;
    pi.layout = layout;
    pi.renderPass = ctx.renderPass();
    VkPipeline p = VK_NULL_HANDLE;
    // A validation error or a bad module must not kill the process here, hence no VK_CHECK.
    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pi, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    vkDestroyShaderModule(dev, vm, nullptr);
    vkDestroyShaderModule(dev, fm, nullptr);
    return p;
}

bool Renderer::replaceShader(ShaderSlot slot, const uint32_t* vs, size_t vsSize, const uint32_t* fs, size_t fsSize) {
    if (!ctx_ || !layout_) return false;
    static const VkVertexInputAttributeDescription kCellAttrs[] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(CellInstance, x)},
        {1, 0, VK_FORMAT_R32_SFLOAT, offsetof(CellInstance, radius)},
        {2, 0, VK_FORMAT_R32_SFLOAT, offsetof(CellInstance, angle)},
        {3, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(CellInstance, color)},
        {4, 0, VK_FORMAT_R32_UINT, offsetof(CellInstance, typeFlags)},
        {5, 0, VK_FORMAT_R32_SFLOAT, offsetof(CellInstance, p0)},
        {6, 0, VK_FORMAT_R32_SFLOAT, offsetof(CellInstance, p1)},
        {7, 0, VK_FORMAT_R32G32B32A32_UINT, offsetof(CellInstance, shape)},
        {8, 0, VK_FORMAT_R32G32B32A32_UINT, offsetof(CellInstance, shape) + 16},
    };
    static const VkVertexInputAttributeDescription kLineAttrs[] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(LineInstance, ax)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(LineInstance, bx)},
        {2, 0, VK_FORMAT_R32_SFLOAT, offsetof(LineInstance, width)},
        {3, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(LineInstance, color)},
        {4, 0, VK_FORMAT_R32_SFLOAT, offsetof(LineInstance, pinch)},
    };
    static const VkVertexInputAttributeDescription kPartAttrs[] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ParticleInstance, x)},
        {1, 0, VK_FORMAT_R32_SFLOAT, offsetof(ParticleInstance, radius)},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(ParticleInstance, color)},
        {3, 0, VK_FORMAT_R32_UINT, offsetof(ParticleInstance, kind)},
        {4, 0, VK_FORMAT_R32_SFLOAT, offsetof(ParticleInstance, angle)},
    };
    const VkVertexInputAttributeDescription* attrs = nullptr;
    uint32_t attrCount = 0, stride = 0;
    VkPipeline* target = nullptr;
    switch (slot) {
        case ShaderSlot::Dish: target = &dishPipe_; break;
        case ShaderSlot::Cell: attrs = kCellAttrs; attrCount = 9; stride = sizeof(CellInstance); target = &cellPipe_; break;
        case ShaderSlot::Line: attrs = kLineAttrs; attrCount = 5; stride = sizeof(LineInstance); target = &linePipe_; break;
        case ShaderSlot::Particle:
            attrs = kPartAttrs; attrCount = 5; stride = sizeof(ParticleInstance); target = &particlePipe_; break;
    }
    if (!target) return false;
    VkPipeline fresh = tryMakePipeline(*ctx_, layout_, vs, vsSize, fs, fsSize, attrs, attrCount, stride);
    if (fresh == VK_NULL_HANDLE) return false;  // the working pipeline stays in place
    if (*target) vkDestroyPipeline(ctx_->device(), *target, nullptr);
    *target = fresh;
    return true;
}

VkBuffer Renderer::upload(DynBuffer& db, const void* data, size_t bytes) {
    GpuBuffer& b = db.buf[ctx_->frameIndex()];
    if (b.size < bytes) {
        // This frame slot's fence has been waited on, so its old buffer is no longer in use.
        ctx_->destroyBuffer(b);
        VkDeviceSize cap = 64 * 1024;
        while (cap < bytes) cap *= 2;
        b = ctx_->createHostBuffer(cap, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    }
    std::memcpy(b.mapped, data, bytes);
    return b.buffer;
}

void Renderer::draw(const RenderLists& lists, const SceneParams& s, int slot, VkExtent2D ext) {
    VkCommandBuffer cmd = ctx_->cmd();

    PushConstants pc{};
    pc.cam[0] = s.camX;
    pc.cam[1] = s.camY;
    pc.cam[2] = 2.0f * s.zoom / (float)ext.width;
    pc.cam[3] = -2.0f * s.zoom / (float)ext.height;
    pc.params[0] = s.time;
    pc.params[1] = 1.0f / s.zoom;
    pc.params[2] = s.dishRadius;
    pc.params[3] = s.light;
    pc.extra[0] = s.lightAngle;
    pc.extra[1] = s.gridAlpha;
    pc.extra[2] = s.lightRange;
    pc.extra[3] = s.theme;
    pc.extra2[0] = s.night;
    pc.extra2[1] = pc.extra2[2] = pc.extra2[3] = 0;

    auto push = [&] {
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    };

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, dishPipe_);
    push();
    vkCmdDraw(cmd, 3, 1, 0, 0);

    VkDeviceSize zero = 0;
    auto drawInstances = [&](VkPipeline pipe, DynBuffer& db, const void* data, size_t count, size_t stride) {
        if (!count) return;
        VkBuffer vb = upload(db, data, count * stride);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        push();
        vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &zero);
        vkCmdDraw(cmd, 6, (uint32_t)count, 0, 0);
    };

    drawInstances(particlePipe_, foodBuf_[slot], lists.food.data(), lists.food.size(), sizeof(ParticleInstance));
    // Thousands of crumbs (zoomed far out): the rest goes into a pass of its own (see VkContext::splitMainPass).
    if (splitAfterFood && lists.food.size() > 1500) ctx_->splitMainPass();
    // Adhesin slime lies between the membranes, under the cells.
    drawInstances(linePipe_, lineBuf_[slot], lists.lines.data(), lists.lines.size(), sizeof(LineInstance));
    drawInstances(cellPipe_, cellBuf_[slot], lists.cells.data(), lists.cells.size(), sizeof(CellInstance));
    lastCellData = cellBuf_[slot].buf[ctx_->frameIndex()].mapped;
    lastCellBytes = lists.cells.size() * sizeof(CellInstance);
    lastCellBuffer = cellBuf_[slot].buf[ctx_->frameIndex()].buffer;
    drawInstances(linePipe_, fibreBuf_[slot], lists.fibres.data(), lists.fibres.size(), sizeof(LineInstance));
    drawInstances(particlePipe_, overlayBuf_[slot], lists.overlay.data(), lists.overlay.size(), sizeof(ParticleInstance));
}

void Renderer::shutdown() {
    if (!ctx_) return;
    VkDevice dev = ctx_->device();
    vkDeviceWaitIdle(dev);
    for (int slot = 0; slot < 2; ++slot)
        for (DynBuffer* db : {&cellBuf_[slot], &lineBuf_[slot], &fibreBuf_[slot], &foodBuf_[slot], &overlayBuf_[slot]})
            for (auto& b : db->buf) ctx_->destroyBuffer(b);
    destroyTarget(preview_);
    if (offscreenPass_) vkDestroyRenderPass(dev, offscreenPass_, nullptr);
    for (VkPipeline p : {dishPipe_, cellPipe_, linePipe_, particlePipe_})
        if (p) vkDestroyPipeline(dev, p, nullptr);
    if (layout_) vkDestroyPipelineLayout(dev, layout_, nullptr);
    ctx_ = nullptr;
}
