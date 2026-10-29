/* The on-screen window (runner --window): both screens, scaled to fit the
 * window with the console's proportions, and the keyboard and mouse as 3DS
 * input. Native Windows builds only (cross-compiled from WSL); elsewhere
 * window_open says so and the rest does nothing.
 *
 * A thread of its own owns the window and its message loop (Windows wants
 * that). Each vblank the emulation thread copies the frame into a staging
 * buffer and has Vulkan copy it to an image, then blit that onto the
 * swapchain image and present. Vulkan comes from the driver's
 * vulkan-1.dll, loaded at run time, so no import library is needed.
 *
 * Keys: arrows = D-pad, X = A, Z = B, S = X, A = Y, Q = L, W = R,
 * Enter = START, Backspace = SELECT. The left mouse button on the bottom
 * screen touches it. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel.h"

#ifndef _WIN32

int window_open(int scale)
{
    (void)scale;
    fprintf(stderr, "hle: --window needs a native Windows build\n");
    return 0;
}
void window_present(const uint8_t *rgb) { (void)rgb; }
uint32_t window_keys(void) { return 0; }
int window_touch(uint32_t *x, uint32_t *y) { (void)x; (void)y; return 0; }
int window_closed(void) { return 0; }

#else

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>

#define FRAME_W 400
#define FRAME_H 480

static HWND hwnd;
static HANDLE ready;
static int scale_asked;
static volatile LONG held_keys, touch_down, touch_xy, closed;

/* ---------------------------------------------------------------- input */

static uint32_t key_bit(WPARAM vk)
{
    switch (vk) {
    case 'X':       return 1u << 0;     /* A */
    case 'Z':       return 1u << 1;     /* B */
    case VK_BACK:   return 1u << 2;     /* SELECT */
    case VK_RETURN: return 1u << 3;     /* START */
    case VK_RIGHT:  return 1u << 4;
    case VK_LEFT:   return 1u << 5;
    case VK_UP:     return 1u << 6;
    case VK_DOWN:   return 1u << 7;
    case 'W':       return 1u << 8;     /* R */
    case 'Q':       return 1u << 9;     /* L */
    case 'S':       return 1u << 10;    /* X */
    case 'A':       return 1u << 11;    /* Y */
    default:        return 0;
    }
}

/* Where the frame goes in a client area of w x h: as large as fits, with
   the console's proportions, centred. */
static void fit(int w, int h, int *x0, int *y0, int *dw, int *dh)
{
    double s = (double)w / FRAME_W < (double)h / FRAME_H ? (double)w / FRAME_W : (double)h / FRAME_H;
    *dw = (int)(FRAME_W * s);
    *dh = (int)(FRAME_H * s);
    *x0 = (w - *dw) / 2;
    *y0 = (h - *dh) / 2;
}

static void mouse(int mx, int my, int down)
{
    RECT r;
    GetClientRect(hwnd, &r);
    int x0, y0, dw, dh;
    fit(r.right, r.bottom, &x0, &y0, &dw, &dh);
    if (dw <= 0 || dh <= 0)
        return;
    int fx = (mx - x0) * FRAME_W / dw, fy = (my - y0) * FRAME_H / dh;
    /* The bottom screen: 320x240, centred under the top one. */
    if (down && fy >= 240 && fy < FRAME_H && fx >= 40 && fx < 360) {
        InterlockedExchange(&touch_xy, (LONG)((uint32_t)(fx - 40) | (uint32_t)(fy - 240) << 16));
        InterlockedExchange(&touch_down, 1);
    } else {
        InterlockedExchange(&touch_down, 0);
    }
}

static LRESULT CALLBACK window_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_KEYDOWN:
        InterlockedOr(&held_keys, (LONG)key_bit(wp));
        return 0;
    case WM_KEYUP:
        InterlockedAnd(&held_keys, (LONG)~key_bit(wp));
        return 0;
    case WM_KILLFOCUS:
        InterlockedExchange(&held_keys, 0);     /* no key stays stuck */
        return 0;
    case WM_LBUTTONDOWN:
        SetCapture(w);
        mouse((short)LOWORD(lp), (short)HIWORD(lp), 1);
        return 0;
    case WM_MOUSEMOVE:
        if (wp & MK_LBUTTON)
            mouse((short)LOWORD(lp), (short)HIWORD(lp), 1);
        return 0;
    case WM_LBUTTONUP:
        ReleaseCapture();
        mouse(0, 0, 0);
        return 0;
    case WM_CLOSE:
        InterlockedExchange(&closed, 1);
        fprintf(stderr, "hle: window closed\n");
        exit(0);
    }
    return DefWindowProcA(w, msg, wp, lp);
}

static DWORD WINAPI window_thread(LPVOID arg)
{
    (void)arg;
    HINSTANCE inst = GetModuleHandleA(NULL);
    WNDCLASSA wc = { .lpfnWndProc = window_proc, .hInstance = inst, .lpszClassName = "any3ds",
                     .hCursor = LoadCursor(NULL, IDC_ARROW) };
    RegisterClassA(&wc);
    RECT r = { 0, 0, FRAME_W * scale_asked, FRAME_H * scale_asked };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowA("any3ds", "any3ds", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                         r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    if (hwnd)
        ShowWindow(hwnd, SW_SHOW);
    SetEvent(ready);
    if (!hwnd)
        return 1;
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}

uint32_t window_keys(void) { return (uint32_t)held_keys; }
int window_closed(void) { return (int)closed; }

int window_touch(uint32_t *x, uint32_t *y)
{
    if (!touch_down)
        return 0;
    uint32_t v = (uint32_t)touch_xy;
    *x = v & 0xFFFF;
    *y = v >> 16;
    return 1;
}

/* ---------------------------------------------------------------- Vulkan */

static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
#define VKFN(name) static PFN_##name name;
#define INSTANCE_FNS(X) X(vkCreateInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) X(vkCreateDevice) \
    X(vkGetDeviceProcAddr) X(vkCreateWin32SurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)
#define DEVICE_FNS(X) X(vkGetDeviceQueue) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) \
    X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkCreateCommandPool) \
    X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) \
    X(vkQueueSubmit) X(vkCreateFence) X(vkWaitForFences) X(vkResetFences) X(vkCreateSemaphore) \
    X(vkCreateBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkBindBufferMemory) \
    X(vkMapMemory) X(vkCreateImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) \
    X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkCmdBlitImage) X(vkCmdClearColorImage) \
    X(vkDeviceWaitIdle)
INSTANCE_FNS(VKFN)
DEVICE_FNS(VKFN)

static VkInstance instance;
static VkPhysicalDevice gpu;
static VkDevice device;
static VkQueue queue;
static uint32_t queue_family;
static VkSurfaceKHR surface;
static VkSwapchainKHR swapchain;
static VkImage swap_images[8];
static uint32_t nswap;
static VkExtent2D swap_extent;
static int swap_stale;
static VkCommandBuffer cmd;
static VkFence frame_done;
static VkSemaphore acquired, rendered;
static VkBuffer staging;
static uint8_t *staging_ptr;
static VkImage frame_image;

static int fail(const char *what, VkResult r)
{
    fprintf(stderr, "hle: window: %s failed (VkResult %d)\n", what, (int)r);
    return 0;
}

static int memory_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties p;
    vkGetPhysicalDeviceMemoryProperties(gpu, &p);
    for (uint32_t i = 0; i < p.memoryTypeCount; i++)
        if ((bits >> i & 1) && (p.memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    return -1;
}

static int pick_gpu(void)
{
    VkPhysicalDevice devs[8];
    uint32_t n = 8;
    vkEnumeratePhysicalDevices(instance, &n, devs);
    int best = -1, best_discrete = 0;
    uint32_t best_family = 0;
    for (uint32_t i = 0; i < n; i++) {
        VkQueueFamilyProperties fam[16];
        uint32_t nf = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nf, fam);
        for (uint32_t f = 0; f < nf; f++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], f, surface, &present);
            if (!(fam[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present)
                continue;
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(devs[i], &p);
            int discrete = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
            if (best < 0 || (discrete && !best_discrete)) {
                best = (int)i;
                best_discrete = discrete;
                best_family = f;
            }
            break;
        }
    }
    if (best < 0)
        return 0;
    gpu = devs[best];
    queue_family = best_family;
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(gpu, &p);
    fprintf(stderr, "hle: window: drawing with %s\n", p.deviceName);
    return 1;
}

static int make_swapchain(void)
{
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps);
    swap_extent = caps.currentExtent;
    if (swap_extent.width == 0 || swap_extent.height == 0)
        return 0;                       /* minimised: try again later */
    VkSurfaceFormatKHR formats[32];
    uint32_t nformats = 32;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &nformats, formats);
    VkSurfaceFormatKHR format = formats[0];
    for (uint32_t i = 0; i < nformats; i++)
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
            format = formats[i];
            break;
        }
    /* Don't wait for the monitor: --speed paces the game itself. */
    VkPresentModeKHR modes[8], mode = VK_PRESENT_MODE_FIFO_KHR;
    uint32_t nmodes = 8;
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, &nmodes, modes);
    for (uint32_t i = 0; i < nmodes; i++)
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR || (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR && mode != VK_PRESENT_MODE_MAILBOX_KHR))
            mode = modes[i];
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount)
        count = caps.maxImageCount;
    VkSwapchainKHR old = swapchain;
    VkSwapchainCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surface, .minImageCount = count,
        .imageFormat = format.format, .imageColorSpace = format.colorSpace, .imageExtent = swap_extent,
        .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = mode, .clipped = VK_TRUE,
        .oldSwapchain = old,
    };
    VkResult r = vkCreateSwapchainKHR(device, &sci, NULL, &swapchain);
    if (old)
        vkDestroySwapchainKHR(device, old, NULL);
    if (r != VK_SUCCESS) {
        swapchain = VK_NULL_HANDLE;
        return fail("vkCreateSwapchainKHR", r);
    }
    nswap = 8;
    vkGetSwapchainImagesKHR(device, swapchain, &nswap, swap_images);
    swap_stale = 0;
    return 1;
}

static int vulkan_init(void)
{
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    if (!lib)
        return fail("loading vulkan-1.dll", VK_ERROR_INITIALIZATION_FAILED);
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)(void (*)(void))GetProcAddress(lib, "vkGetInstanceProcAddr");
    vkCreateInstance = (PFN_vkCreateInstance)vkGetInstanceProcAddr(NULL, "vkCreateInstance");
    const char *ext[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "any3ds",
                              .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                                 .enabledExtensionCount = 2, .ppEnabledExtensionNames = ext };
    VkResult r = vkCreateInstance(&ici, NULL, &instance);
    if (r != VK_SUCCESS)
        return fail("vkCreateInstance", r);
#define LOAD_INSTANCE(name) name = (PFN_##name)vkGetInstanceProcAddr(instance, #name);
    INSTANCE_FNS(LOAD_INSTANCE)
    VkWin32SurfaceCreateInfoKHR wci = { .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
                                        .hinstance = GetModuleHandleA(NULL), .hwnd = hwnd };
    if ((r = vkCreateWin32SurfaceKHR(instance, &wci, NULL, &surface)) != VK_SUCCESS)
        return fail("vkCreateWin32SurfaceKHR", r);
    if (!pick_gpu())
        return fail("finding a GPU that can draw to the window", VK_ERROR_INITIALIZATION_FAILED);

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = queue_family,
                                    .queueCount = 1, .pQueuePriorities = &priority };
    const char *dext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                               .pQueueCreateInfos = &qci, .enabledExtensionCount = 1, .ppEnabledExtensionNames = dext };
    if ((r = vkCreateDevice(gpu, &dci, NULL, &device)) != VK_SUCCESS)
        return fail("vkCreateDevice", r);
#define LOAD_DEVICE(name) name = (PFN_##name)vkGetDeviceProcAddr(device, #name);
    DEVICE_FNS(LOAD_DEVICE)
    vkGetDeviceQueue(device, queue_family, 0, &queue);

    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                    .queueFamilyIndex = queue_family };
    VkCommandPool pool;
    vkCreateCommandPool(device, &pci, NULL, &pool);
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    vkAllocateCommandBuffers(device, &cai, &cmd);
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    vkCreateFence(device, &fci, NULL, &frame_done);
    VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    vkCreateSemaphore(device, &sci, NULL, &acquired);
    vkCreateSemaphore(device, &sci, NULL, &rendered);

    /* The frame's way in: a mapped staging buffer, then an image to blit from. */
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = FRAME_W * FRAME_H * 4,
                               .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
    vkCreateBuffer(device, &bci, NULL, &staging);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, staging, &req);
    int type = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                 .memoryTypeIndex = (uint32_t)type };
    VkDeviceMemory mem;
    if (type < 0 || vkAllocateMemory(device, &mai, NULL, &mem) != VK_SUCCESS)
        return fail("allocating the staging buffer", VK_ERROR_OUT_OF_DEVICE_MEMORY);
    vkBindBufferMemory(device, staging, mem, 0);
    vkMapMemory(device, mem, 0, VK_WHOLE_SIZE, 0, (void **)&staging_ptr);

    VkImageCreateInfo ici2 = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                               .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { FRAME_W, FRAME_H, 1 }, .mipLevels = 1,
                               .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                               .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                               .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    vkCreateImage(device, &ici2, NULL, &frame_image);
    vkGetImageMemoryRequirements(device, frame_image, &req);
    type = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)type;
    if (type < 0 || vkAllocateMemory(device, &mai, NULL, &mem) != VK_SUCCESS)
        return fail("allocating the frame image", VK_ERROR_OUT_OF_DEVICE_MEMORY);
    vkBindImageMemory(device, frame_image, mem, 0);
    make_swapchain();
    return 1;
}

static void barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst)
{
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = src, .dstAccessMask = dst,
                               .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
                               .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

void window_present(const uint8_t *rgb)
{
    if (!device || !rgb)
        return;
    if ((!swapchain || swap_stale) && (vkDeviceWaitIdle(device), !make_swapchain()))
        return;
    vkWaitForFences(device, 1, &frame_done, VK_TRUE, UINT64_MAX);
    uint32_t index;
    VkResult r = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        swap_stale = 1;
        return;
    }
    vkResetFences(device, 1, &frame_done);
    for (int i = 0; i < FRAME_W * FRAME_H; i++) {
        staging_ptr[4 * i] = rgb[3 * i];
        staging_ptr[4 * i + 1] = rgb[3 * i + 1];
        staging_ptr[4 * i + 2] = rgb[3 * i + 2];
        staging_ptr[4 * i + 3] = 255;
    }

    VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkResetCommandBuffer(cmd, 0);
    vkBeginCommandBuffer(cmd, &begin);
    barrier(frame_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy copy = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { FRAME_W, FRAME_H, 1 } };
    vkCmdCopyBufferToImage(cmd, staging, frame_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier(frame_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);

    VkImage target = swap_images[index];
    barrier(target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkClearColorValue black = { .float32 = { 0, 0, 0, 1 } };
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    int x0, y0, dw, dh;
    fit((int)swap_extent.width, (int)swap_extent.height, &x0, &y0, &dw, &dh);
    VkImageBlit blit = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffsets = { { 0, 0, 0 }, { FRAME_W, FRAME_H, 1 } },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets = { { x0, y0, 0 }, { x0 + dw, y0 + dh, 1 } },
    };
    vkCmdBlitImage(cmd, frame_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blit, VK_FILTER_NEAREST);
    barrier(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    vkEndCommandBuffer(cmd);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &acquired,
                            .pWaitDstStageMask = &wait_stage, .commandBufferCount = 1, .pCommandBuffers = &cmd,
                            .signalSemaphoreCount = 1, .pSignalSemaphores = &rendered };
    vkQueueSubmit(queue, 1, &submit, frame_done);
    VkPresentInfoKHR present = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                                 .pWaitSemaphores = &rendered, .swapchainCount = 1, .pSwapchains = &swapchain,
                                 .pImageIndices = &index };
    r = vkQueuePresentKHR(queue, &present);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
        swap_stale = 1;
}

int window_open(int scale)
{
    scale_asked = scale < 1 ? 2 : scale;
    ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    CreateThread(NULL, 0, window_thread, NULL, 0, NULL);
    WaitForSingleObject(ready, INFINITE);
    if (!hwnd) {
        fprintf(stderr, "hle: window: could not create the window\n");
        return 0;
    }
    return vulkan_init();
}

#endif
