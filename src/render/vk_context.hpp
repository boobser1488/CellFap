#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

struct GLFWwindow;

[[noreturn]] void fatalError(const char* msg);  // shows a message box (no console in release) and aborts
void vkCheck(VkResult r, const char* what);
#define VK_CHECK(x) vkCheck((x), #x)

struct GpuBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void* mapped = nullptr;
};

// Owns the Vulkan instance/device/swapchain and per-frame synchronisation.
class VkContext {
public:
    static constexpr uint32_t kFramesInFlight = 2;

    bool init(GLFWwindow* window, bool enableValidation);
    void shutdown();

    // Returns false when the swapchain had to be recreated and the frame must be skipped.
    bool beginFrame();
    void beginMainPass(const float clearColor[4]);
    void splitMainPass();  // end the main pass and resume drawing on the same image in a new one
    void endFrame();

    void setVsync(bool on);
    bool vsync() const { return vsync_; }
    void requestResize() { resizeRequested_ = true; }
    // Android: the window goes away in the background. Drop the swapchain and surface, and rebuild them later.
    void releaseSurface();
    void restoreSurface();

    // Screenshot: the next endFrame() copies the presented image to host memory.
    void requestCapture() { captureRequested_ = true; }
    // Returns true once per completed capture; rgb receives tightly packed 8-bit RGB rows.
    bool takeCapture(std::vector<uint8_t>& rgb, uint32_t& w, uint32_t& h);

    GpuBuffer createHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage);
    void destroyBuffer(GpuBuffer& b);
    VkShaderModule createShader(const uint32_t* code, size_t sizeBytes);

    VkInstance instance() const { return instance_; }
    VkPhysicalDevice physicalDevice() const { return physical_; }
    VkDevice device() const { return device_; }
    VkQueue queue() const { return queue_; }
    uint32_t queueFamily() const { return queueFamily_; }
    VkRenderPass renderPass() const { return renderPass_; }
    VkDescriptorPool descriptorPool() const { return descriptorPool_; }
    VkCommandBuffer cmd() const { return frames_[frameIndex_].cmd; }
    uint32_t frameIndex() const { return frameIndex_; }
    uint32_t imageCount() const { return (uint32_t)swapImages_.size(); }
    uint32_t minImageCount() const { return minImageCount_; }
    VkExtent2D extent() const { return extent_; }
    const char* gpuName() const { return gpuName_; }
    VkFormat swapFormat() const { return swapFormat_; }
    uint32_t findMemoryType(uint32_t bits, VkMemoryPropertyFlags props);

private:
    bool createInstance(bool validation);
    bool pickDevice();
    bool createSwapchain();
    void destroySwapchain();
    void recreateSwapchain();

    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
    };

    GLFWwindow* window_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    VkPhysicalDeviceMemoryProperties memProps_{};
    char gpuName_[256] = {};

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapFormat_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D extent_{};
    uint32_t minImageCount_ = 2;
    std::vector<VkImage> swapImages_;
    std::vector<VkImageView> swapViews_;
    std::vector<VkFramebuffer> framebuffers_;
    std::vector<VkSemaphore> renderFinished_;   // one per swapchain image
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkRenderPass continuePass_ = VK_NULL_HANDLE;  // the main pass resumed on the drawn image (splitMainPass)
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;

    Frame frames_[kFramesInFlight];
    uint32_t frameIndex_ = 0;
    uint32_t imageIndex_ = 0;
    bool vsync_ = true;
    bool resizeRequested_ = false;

    bool canCapture_ = false;
    bool captureRequested_ = false;
    bool captureReady_ = false;
    GpuBuffer captureBuf_;
    std::vector<uint8_t> captureRgb_;
    uint32_t captureW_ = 0, captureH_ = 0;
};
