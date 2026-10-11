#include "vk_context.hpp"

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef __ANDROID__
#include <android/log.h>
#endif

#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall MessageBoxA(void*, const char*, const char*, unsigned);
#endif

void fatalError(const char* msg) {
    std::fprintf(stderr, "%s\n", msg);
#ifdef _WIN32
    // The release build has no console (-mwindows), so stderr alone would be invisible.
    MessageBoxA(nullptr, msg, "CellFap", 0x10 /*MB_ICONERROR*/);
#elif defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_ERROR, "CellFap", "%s", msg);
#endif
    std::abort();
}

void vkCheck(VkResult r, const char* what) {
    if (r == VK_SUCCESS) return;
    if (r < 0) {
        char buf[512];
        std::snprintf(buf, sizeof(buf), "Vulkan error %d in\n%s", (int)r, what);
        fatalError(buf);
    }
    std::fprintf(stderr, "Vulkan warning %d in %s\n", (int)r, what);
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                    VkDebugUtilsMessageTypeFlagsEXT,
                                                    const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::fprintf(stderr, "[vulkan] %s\n", data->pMessage);
    return VK_FALSE;
}

bool VkContext::init(GLFWwindow* window, bool enableValidation) {
    window_ = window;
    if (!createInstance(enableValidation)) return false;
    VK_CHECK(glfwCreateWindowSurface(instance_, window_, nullptr, &surface_));
    if (!pickDevice()) return false;

    for (auto& f : frames_) {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = queueFamily_;
        VK_CHECK(vkCreateCommandPool(device_, &pci, nullptr, &f.pool));
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(device_, &ai, &f.cmd));
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(device_, &si, nullptr, &f.imageAvailable));
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(device_, &fi, nullptr, &f.fence));
    }

    VkDescriptorPoolSize poolSizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpi.maxSets = 16;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(device_, &dpi, nullptr, &descriptorPool_));

    return createSwapchain();
}

bool VkContext::createInstance(bool validation) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "CellFap";
    app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app.pEngineName = "CellFap";
    app.apiVersion = VK_API_VERSION_1_1;

    uint32_t glfwCount = 0;
    const char** glfwExt = glfwGetRequiredInstanceExtensions(&glfwCount);
    std::vector<const char*> exts(glfwExt, glfwExt + glfwCount);
    std::vector<const char*> layers;

    if (validation) {
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> props(n);
        vkEnumerateInstanceLayerProperties(&n, props.data());
        bool found = false;
        for (auto& p : props)
            if (std::strcmp(p.layerName, "VK_LAYER_KHRONOS_validation") == 0) found = true;
        if (found) {
            layers.push_back("VK_LAYER_KHRONOS_validation");
            exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        } else {
            validation = false;
            std::fprintf(stderr, "Validation layer not available\n");
        }
    }

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = (uint32_t)exts.size();
    ci.ppEnabledExtensionNames = exts.data();
    ci.enabledLayerCount = (uint32_t)layers.size();
    ci.ppEnabledLayerNames = layers.data();
    if (vkCreateInstance(&ci, nullptr, &instance_) != VK_SUCCESS) {
        std::fprintf(stderr, "Failed to create Vulkan instance\n");
        return false;
    }

    if (validation) {
        auto fn = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT");
        if (fn) {
            VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            mi.pfnUserCallback = debugCallback;
            fn(instance_, &mi, nullptr, &messenger_);
        }
    }
    return true;
}

bool VkContext::pickDevice() {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    if (n == 0) {
        std::fprintf(stderr, "No Vulkan devices found\n");
        return false;
    }
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(instance_, &n, devs.data());

    int bestScore = -1;
    for (auto d : devs) {
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qprops(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qprops.data());
        int family = -1;
        for (uint32_t i = 0; i < qn; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface_, &present);
            if ((qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                family = (int)i;
                break;
            }
        }
        if (family < 0) continue;

        uint32_t en = 0;
        vkEnumerateDeviceExtensionProperties(d, nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> eprops(en);
        vkEnumerateDeviceExtensionProperties(d, nullptr, &en, eprops.data());
        bool hasSwap = false;
        for (auto& e : eprops)
            if (std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) hasSwap = true;
        if (!hasSwap) continue;

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(d, &props);
        int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 1000
                    : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 500
                                                                                  : 10;
        if (score > bestScore) {
            bestScore = score;
            physical_ = d;
            queueFamily_ = (uint32_t)family;
            std::snprintf(gpuName_, sizeof(gpuName_), "%s", props.deviceName);
        }
    }
    if (!physical_) {
        std::fprintf(stderr, "No suitable Vulkan device\n");
        return false;
    }
    vkGetPhysicalDeviceMemoryProperties(physical_, &memProps_);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queueFamily_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* devExt[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = devExt;
    VK_CHECK(vkCreateDevice(physical_, &dci, nullptr, &device_));
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
    return true;
}

bool VkContext::createSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    int w = 0, h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    while (w == 0 || h == 0) {  // minimised
        glfwWaitEvents();
        glfwGetFramebufferSize(window_, &w, &h);
    }
    // Query after the wait: capabilities of a minimised window report a 0x0 extent.
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps));
#ifdef __ANDROID__
    // Android reports the surface extent in the display's native (portrait) orientation. With the identity transform the
    // images must have the window's present shape, or the compositor stretches a portrait picture over a phone held
    // sideways (the game "did not notice" the rotation).
    if (true) {
        extent_ = {(uint32_t)w, (uint32_t)h};
    } else
#endif
    if (caps.currentExtent.width != UINT32_MAX) {
        extent_ = caps.currentExtent;
    } else {
        extent_.width = std::clamp((uint32_t)w, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent_.height = std::clamp((uint32_t)h, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent_.width == 0 || extent_.height == 0) return false;

    uint32_t fn = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &fn, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fn);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &fn, formats.data());
    VkSurfaceFormatKHR chosen = formats[0];
    for (auto& f : formats) {
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    }
    swapFormat_ = chosen.format;

    uint32_t pn = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &pn, nullptr);
    std::vector<VkPresentModeKHR> modes(pn);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &pn, modes.data());
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (!vsync_) {
        for (auto m : modes)
            if (m == VK_PRESENT_MODE_MAILBOX_KHR) presentMode = m;
        if (presentMode == VK_PRESENT_MODE_FIFO_KHR)
            for (auto m : modes)
                if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) presentMode = m;
    }

    minImageCount_ = std::max(2u, caps.minImageCount);
    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0) imageCount = std::min(imageCount, caps.maxImageCount);

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface_;
    sci.minImageCount = imageCount;
    sci.imageFormat = chosen.format;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = extent_;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    canCapture_ = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    if (canCapture_) sci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    // The compositor rotates for us when the identity transform is available (no pre-rotation in our shaders).
    sci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                                                                      : caps.currentTransform;
    // Android surfaces often offer only "inherit".
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR))
        for (VkCompositeAlphaFlagBitsKHR a : {VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                                              VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR})
            if (caps.supportedCompositeAlpha & a) {
                sci.compositeAlpha = a;
                break;
            }
    sci.presentMode = presentMode;
    sci.clipped = VK_TRUE;
    VK_CHECK(vkCreateSwapchainKHR(device_, &sci, nullptr, &swapchain_));

    uint32_t n = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &n, nullptr);
    swapImages_.resize(n);
    vkGetSwapchainImagesKHR(device_, swapchain_, &n, swapImages_.data());

    if (!renderPass_) {
        VkAttachmentDescription att{};
        att.format = swapFormat_;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;
        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpi.attachmentCount = 1;
        rpi.pAttachments = &att;
        rpi.subpassCount = 1;
        rpi.pSubpasses = &sub;
        rpi.dependencyCount = 1;
        rpi.pDependencies = &dep;
        VK_CHECK(vkCreateRenderPass(device_, &rpi, nullptr, &renderPass_));
    }

    swapViews_.resize(n);
    framebuffers_.resize(n);
    renderFinished_.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = swapImages_[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = swapFormat_;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(device_, &vi, nullptr, &swapViews_[i]));
        VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbi.renderPass = renderPass_;
        fbi.attachmentCount = 1;
        fbi.pAttachments = &swapViews_[i];
        fbi.width = extent_.width;
        fbi.height = extent_.height;
        fbi.layers = 1;
        VK_CHECK(vkCreateFramebuffer(device_, &fbi, nullptr, &framebuffers_[i]));
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(device_, &si, nullptr, &renderFinished_[i]));
    }
    return true;
}

void VkContext::destroySwapchain() {
    for (auto fb : framebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
    for (auto v : swapViews_) vkDestroyImageView(device_, v, nullptr);
    for (auto s : renderFinished_) vkDestroySemaphore(device_, s, nullptr);
    framebuffers_.clear();
    swapViews_.clear();
    renderFinished_.clear();
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

void VkContext::recreateSwapchain() {
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "CellFap", "swapchain recreated (was %ux%u)", extent_.width, extent_.height);
#endif
    vkDeviceWaitIdle(device_);
    destroySwapchain();
    // A transient 0x0 extent (e.g. mid-minimise) makes creation fail; wait for a usable size.
    while (!createSwapchain()) {
        destroySwapchain();
        glfwWaitEvents();
    }
    resizeRequested_ = false;
}

void VkContext::releaseSurface() {
    if (!surface_) return;
    vkDeviceWaitIdle(device_);
    destroySwapchain();
    vkDestroySurfaceKHR(instance_, surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
}

void VkContext::restoreSurface() {
    if (surface_) return;
    VK_CHECK(glfwCreateWindowSurface(instance_, window_, nullptr, &surface_));
    VkBool32 present = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(physical_, queueFamily_, surface_, &present);
    if (!present) fatalError("Новое окно не поддерживает вывод изображения");
    while (!createSwapchain()) {
        destroySwapchain();
        glfwWaitEvents();
    }
    resizeRequested_ = false;
}

void VkContext::setVsync(bool on) {
    if (on == vsync_) return;
    vsync_ = on;
    resizeRequested_ = true;
}

bool VkContext::beginFrame() {
    if (resizeRequested_) recreateSwapchain();
    Frame& f = frames_[frameIndex_];
    VK_CHECK(vkWaitForFences(device_, 1, &f.fence, VK_TRUE, UINT64_MAX));

#ifdef __ANDROID__
    {  // turned: the window changed shape (the size event may come before the surface did)
        int w = 0, h = 0;
        glfwGetFramebufferSize(window_, &w, &h);
        if (w > 0 && h > 0 && ((uint32_t)w != extent_.width || (uint32_t)h != extent_.height)) {
            recreateSwapchain();
            return false;
        }
    }
#endif
    VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, f.imageAvailable, VK_NULL_HANDLE, &imageIndex_);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        return false;
    }
#ifdef __ANDROID__
    // With the identity transform on a rotated display Android always reports "suboptimal": that is expected.
    if (r == VK_SUBOPTIMAL_KHR) r = VK_SUCCESS;
#endif
    if (r == VK_SUBOPTIMAL_KHR) resizeRequested_ = true;
    else VK_CHECK(r);

    VK_CHECK(vkResetFences(device_, 1, &f.fence));
    VK_CHECK(vkResetCommandPool(device_, f.pool, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(f.cmd, &bi));
    return true;
}

void VkContext::beginMainPass(const float clearColor[4]) {
    VkClearValue clear{};
    std::memcpy(clear.color.float32, clearColor, sizeof(float) * 4);
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = renderPass_;
    rbi.framebuffer = framebuffers_[imageIndex_];
    rbi.renderArea.extent = extent_;
    rbi.clearValueCount = 1;
    rbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd(), &rbi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{0, 0, (float)extent_.width, (float)extent_.height, 0, 1};
    VkRect2D sc{{0, 0}, extent_};
    vkCmdSetViewport(cmd(), 0, 1, &vp);
    vkCmdSetScissor(cmd(), 0, 1, &sc);
}

void VkContext::endFrame() {
    Frame& f = frames_[frameIndex_];
    vkCmdEndRenderPass(f.cmd);

    const bool capturing = captureRequested_ && canCapture_;
    if (capturing) {
        VkDeviceSize bytes = (VkDeviceSize)extent_.width * extent_.height * 4;
        if (captureBuf_.size < bytes) {
            destroyBuffer(captureBuf_);
            captureBuf_ = createHostBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        }
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = swapImages_[imageIndex_];
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &b);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {extent_.width, extent_.height, 1};
        vkCmdCopyImageToBuffer(f.cmd, b.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, captureBuf_.buffer, 1, &region);
        b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.dstAccessMask = 0;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr,
                             1, &b);
    }
    VK_CHECK(vkEndCommandBuffer(f.cmd));

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &f.imageAvailable;
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &renderFinished_[imageIndex_];
    VK_CHECK(vkQueueSubmit(queue_, 1, &si, f.fence));

    if (capturing) {
        VK_CHECK(vkWaitForFences(device_, 1, &f.fence, VK_TRUE, UINT64_MAX));
        captureW_ = extent_.width;
        captureH_ = extent_.height;
        captureRgb_.resize((size_t)captureW_ * captureH_ * 3);
        const uint8_t* src = (const uint8_t*)captureBuf_.mapped;
        bool bgra = swapFormat_ == VK_FORMAT_B8G8R8A8_UNORM || swapFormat_ == VK_FORMAT_B8G8R8A8_SRGB;
        for (size_t i = 0, n = (size_t)captureW_ * captureH_; i < n; ++i) {
            captureRgb_[i * 3 + 0] = src[i * 4 + (bgra ? 2 : 0)];
            captureRgb_[i * 3 + 1] = src[i * 4 + 1];
            captureRgb_[i * 3 + 2] = src[i * 4 + (bgra ? 0 : 2)];
        }
        captureReady_ = true;
        captureRequested_ = false;
    }

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &renderFinished_[imageIndex_];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &imageIndex_;
    VkResult r = vkQueuePresentKHR(queue_, &pi);
#ifdef __ANDROID__
    if (r == VK_SUBOPTIMAL_KHR) r = VK_SUCCESS;  // see beginFrame
#endif
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) resizeRequested_ = true;
    else VK_CHECK(r);

    frameIndex_ = (frameIndex_ + 1) % kFramesInFlight;
}

uint32_t VkContext::findMemoryType(uint32_t bits, VkMemoryPropertyFlags props) {
    for (uint32_t i = 0; i < memProps_.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memProps_.memoryTypes[i].propertyFlags & props) == props) return i;
    fatalError("No suitable Vulkan memory type");
}

GpuBuffer VkContext::createHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage) {
    GpuBuffer b;
    b.size = size;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(device_, &bci, nullptr, &b.buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, b.buffer, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex =
        findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(device_, &mai, nullptr, &b.memory));
    VK_CHECK(vkBindBufferMemory(device_, b.buffer, b.memory, 0));
    VK_CHECK(vkMapMemory(device_, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped));
    return b;
}

void VkContext::destroyBuffer(GpuBuffer& b) {
    if (b.memory) {
        vkUnmapMemory(device_, b.memory);
        vkFreeMemory(device_, b.memory, nullptr);
    }
    if (b.buffer) vkDestroyBuffer(device_, b.buffer, nullptr);
    b = GpuBuffer{};
}

VkShaderModule VkContext::createShader(const uint32_t* code, size_t sizeBytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = sizeBytes;
    ci.pCode = code;
    VkShaderModule m;
    VK_CHECK(vkCreateShaderModule(device_, &ci, nullptr, &m));
    return m;
}

bool VkContext::takeCapture(std::vector<uint8_t>& rgb, uint32_t& w, uint32_t& h) {
    if (!captureReady_) return false;
    rgb.swap(captureRgb_);
    w = captureW_;
    h = captureH_;
    captureReady_ = false;
    return true;
}

void VkContext::shutdown() {
    if (!device_) return;
    vkDeviceWaitIdle(device_);
    destroyBuffer(captureBuf_);
    destroySwapchain();
    if (renderPass_) vkDestroyRenderPass(device_, renderPass_, nullptr);
    if (descriptorPool_) vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
    for (auto& f : frames_) {
        vkDestroyCommandPool(device_, f.pool, nullptr);
        vkDestroySemaphore(device_, f.imageAvailable, nullptr);
        vkDestroyFence(device_, f.fence, nullptr);
    }
    vkDestroyDevice(device_, nullptr);
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (messenger_) {
        auto fn = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT");
        if (fn) fn(instance_, messenger_, nullptr);
    }
    vkDestroyInstance(instance_, nullptr);
    device_ = VK_NULL_HANDLE;
}
