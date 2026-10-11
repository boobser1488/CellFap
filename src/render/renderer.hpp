#pragma once
#include "vk_context.hpp"

#include <vector>

constexpr int kShapeSpokes = 32;

struct CellInstance {
    float x, y, radius, angle;
    uint32_t color;      // RGBA8
    uint32_t typeFlags;  // type | flags << 8 | seed << 16
    float p0, p1;
    // Deformable outline: 32 radial spokes in the cell's local frame, one byte each,
    // radius = 0.4 + byte / 255 (in units of the cell radius).
    uint32_t shape[kShapeSpokes / 4];
};
static_assert(sizeof(CellInstance) == 64);

struct LineInstance {
    float ax, ay, bx, by;
    float width;   // half-width at the ends (world units)
    uint32_t color;
    float pinch;   // 0 = capsule, up to 0.9 = thin in the middle (stretched slime)
    float pad;
};

struct ParticleInstance {
    float x, y, radius;
    uint32_t color;
    // bits 0..7: 0 food, 1 virus, 2 ring (constant 1.5 px line), 3 glow, 4 flat disc, 8 dissolved cloud, 9 spark;
    // bits 8..15: seed
    uint32_t kind;
    float angle = 0;
};

enum CellFlags : uint32_t {
    kFlagSelected = 1,
    kFlagInfected = 2,
    kFlagStarving = 4,
    kFlagHovered = 8,
    kFlagGhost = 16,      // genome editor phantom (child born next stage)
    kFlagModeMark = 32,   // genome editor: cell of the mode being edited
};

struct SceneParams {
    float camX = 0, camY = 0;
    float zoom = 4.0f;  // pixels per world unit
    float time = 0;
    float dishRadius = 100;
    float light = 0.5f;       // 0..1 display brightness of the lit region
    float lightAngle = 1.5708f;
    float lightRange = 1.0f;  // 0..1 (same meaning as the substrate setting)
    float gridAlpha = 0.25f;
    float theme = 1.0f;  // 1 = classic lavender microscope (original game), 0 = dark
    float night = 0.0f;  // 0 = day .. 1 = deep night (day cycle)
};

// Colour target that can be sampled by ImGui (genome-editor preview).
struct OffscreenTarget {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSet texture = VK_NULL_HANDLE;  // ImGui texture id
    VkExtent2D extent{};
};

struct RenderLists {
    std::vector<CellInstance> cells;
    std::vector<LineInstance> lines;
    std::vector<LineInstance> fibres;  // adhesin strands across seams, drawn over the cells
    std::vector<ParticleInstance> food;
    std::vector<ParticleInstance> overlay;
    void clear() {
        cells.clear();
        lines.clear();
        fibres.clear();
        food.clear();
        overlay.clear();
    }
};

class Renderer {
public:
    bool init(VkContext& ctx);
    void shutdown();
    // slot selects an independent set of per-frame instance buffers (0 = main view, 1 = preview).
    void draw(const RenderLists& lists, const SceneParams& scene, int slot, VkExtent2D extent);

    // Offscreen preview target (recreated when the size changes; must be called outside a render pass).
    OffscreenTarget& preview(uint32_t w, uint32_t h);
    void beginPreviewPass(const float clear[4]);
    void endPreviewPass();

private:
    struct DynBuffer {
        GpuBuffer buf[VkContext::kFramesInFlight];
    };
    VkBuffer upload(DynBuffer& db, const void* data, size_t bytes);
    VkPipeline makePipeline(const uint32_t* vs, size_t vsSize, const uint32_t* fs, size_t fsSize,
                            const VkVertexInputAttributeDescription* attrs, uint32_t attrCount, uint32_t stride);

    VkContext* ctx_ = nullptr;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline dishPipe_ = VK_NULL_HANDLE;
    VkPipeline cellPipe_ = VK_NULL_HANDLE;
    VkPipeline linePipe_ = VK_NULL_HANDLE;
    VkPipeline particlePipe_ = VK_NULL_HANDLE;
    void destroyTarget(OffscreenTarget& t);

    DynBuffer cellBuf_[2], lineBuf_[2], fibreBuf_[2], foodBuf_[2], overlayBuf_[2];
    VkRenderPass offscreenPass_ = VK_NULL_HANDLE;
    OffscreenTarget preview_;

public:
    // The cell instances the last draw handed the GPU (test runs compare them with the list).
    const void* lastCellData = nullptr;
    size_t lastCellBytes = 0;
    VkBuffer lastCellBuffer = VK_NULL_HANDLE;
};
