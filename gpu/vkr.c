/* The Vulkan renderer (docs/vulkan-renderer.md, step 2). Triangles that
 * gpu/draw.c has shaded, clipped and queued are drawn on the host GPU
 * instead of by the software rasteriser, batch by batch, when
 * GPU_BACKEND=vulkan and the batch's state is one this file covers
 * (vkr_can_draw). Everything else stays on the software path.
 *
 * For now each batch stands alone: the render target is uploaded from 3DS
 * memory (detiled into an RGBA8 image and a depth/stencil image), drawn
 * into, and read back (retiled) before the next batch. That is slow but
 * always correct, whatever mix of GPU, software and CPU writes a frame
 * has; a surface cache that keeps targets on the GPU comes later.
 *
 * Exactness: the pixel shader (gpu/shaders/draw.frag) runs the combiners in
 * gpu/draw.c's integer arithmetic. The depth buffer is D32_SFLOAT holding
 * the PICA's integer depth times 2^-bits, which floats hold exactly, so
 * depth tests compare the same integers as the software path. Blending is
 * Vulkan's and may round differently by one step; pixels exactly on a
 * triangle's edge can go to the other triangle.
 *
 * GPU_DEVICE=TEXT picks the first device whose name contains TEXT
 * (otherwise a discrete GPU first). Vulkan comes from the loader
 * (libvulkan.so.1 / vulkan-1.dll), loaded at run time. */
#include <inttypes.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "pica.h"
#include "texture.h"
#include "vkr.h"
#include "vkr_shaders.h"

static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
#define VKFN(name) static PFN_##name name;
#define INSTANCE_FNS(X) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceFeatures) X(vkGetPhysicalDeviceFormatProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr)
#define DEVICE_FNS(X) X(vkGetDeviceQueue) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkQueueSubmit) X(vkCreateFence) \
    X(vkWaitForFences) X(vkResetFences) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkCreateImage) X(vkDestroyImage) \
    X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) \
    X(vkCreateRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCreateShaderModule) \
    X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkCreateGraphicsPipelines) \
    X(vkCreateDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCmdPipelineBarrier) \
    X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) \
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers) X(vkCmdSetViewport) \
    X(vkCmdSetScissor) X(vkCmdSetBlendConstants) X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) \
    X(vkCmdSetStencilReference) X(vkCmdDraw) X(vkCreateSampler) X(vkResetDescriptorPool)
static PFN_vkCreateInstance vkCreateInstance;
INSTANCE_FNS(VKFN)
DEVICE_FNS(VKFN)

#define DS_FORMAT VK_FORMAT_D32_SFLOAT_S8_UINT

static int state = -1;                  /* -1: not tried yet, 0: software, 1: Vulkan */
static VkInstance instance;
static VkPhysicalDevice gpu;
static VkDevice device;
static VkQueue queue;
static uint32_t family;
static int has_logic_op;
static void make_dummy(void);
static void tile_tables(void);
static VkCommandPool pool;
static VkCommandBuffer cmd, tcmd;       /* drawing; texture uploads */
static VkFence done, tdone;
static VkSampler sampler;
static VkImageView dummy_view;          /* for units the combiners don't read */
static VkRenderPass pass;
static VkDescriptorSetLayout set_layout;
static VkPipelineLayout layout;
static VkDescriptorPool dpool;          /* one set per batch, freed at each submission */
#define NSETS 1024
static uint32_t nsets;
static VkDeviceSize ubo_align, sbo_align;
static VkShaderModule vert, frag;
static uint64_t batches;                /* drawn here, reported at exit */
#define NWHY 16
static struct { const char *why; uint64_t n; } refusals[NWHY];     /* left to software, by reason */

typedef struct {
    VkBuffer buf;
    VkDeviceMemory mem;
    uint8_t *ptr;
    VkDeviceSize size, used;            /* used: what this submission's batches took */
    VkBufferUsageFlags usage;
} Buffer;
static Buffer ubo = { .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT },
              vbo = { .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT },
              up = { .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT },
              down = { .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT },
              tup = { .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT },
              tables = { .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
/* The lighting and fog state for the shader (draw.frag's Tables): registers
   0x140-0x1D9, FOG_COLOR at 154, the lighting tables from 256, fog's at 6400. */
enum { TAB_FOG_COLOR = 154, TAB_LIGHTING = 256, TAB_FOG = 6400, TAB_WORDS = 6528 };

/* The surface cache: a render target's GPU images (colour RGBA8,
   depth/stencil), kept from batch to batch. While batches are recorded the
   GPU's copy is the newer one; vkr_sync submits them and reads back what
   they drew, whenever someone else may look at 3DS memory (the end of a
   command list, a batch drawn in software, a texture read from a target).
   After that a surface checks its memory against a hash before its next
   use, so what the CPU, the GX engines or the software renderer wrote
   meanwhile is uploaded again. */
typedef struct {
    uint32_t w, h;
    uint8_t *color, *depth;             /* the 3DS memory it mirrors (NULL: none) */
    int color_fmt, depth_fmt;
    VkImage cimg, ds;
    VkDeviceMemory cmem, dmem;
    VkImageView cview, dview;
    VkFramebuffer fb;
    uint64_t used, hash;                /* hash: of its memory when the images last matched it */
    int valid;                          /* the images hold its content */
    int verified;                       /* checked against memory since the last vkr_sync */
    int dirty_color, dirty_depth;       /* drawn on the GPU since: memory is behind */
    VkDeviceSize down_at;               /* where vkr_sync reads it back to */
} Surface;
#define NSURF 8
static Surface surf[NSURF];
static uint64_t clock_;
static int recording;                   /* cmd holds batches not yet submitted */
static Surface *in_pass;                /* the surface whose render pass is open */
static uint64_t epoch = 1;              /* counts submissions */
static uint64_t mem_epoch = 1;          /* counts vkr_sync calls: 3DS memory may have changed at each */
static uint64_t submits, uploads;       /* reported at exit */
/* GPU_STATS=1: where the Vulkan path's time goes, printed with draw.c's
   stats each second (vkr_report). */
enum { VKT_RECORD, VKT_HASH, VKT_UPLOAD, VKT_TEXTURE, VKT_WAIT, VKT_DOWNLOAD, VKT_COUNT };
static uint64_t vt[VKT_COUNT], vt_submits, vt_uploads;
static uint64_t hash_bytes(const uint8_t *p, size_t n, uint64_t h);

/* The pipeline state that can't be dynamic, and its pipelines. */
typedef struct {
    uint32_t blend, rgb_eq, alpha_eq, rgb_src, rgb_dst, alpha_src, alpha_dst, logic, logic_op, mask;
    uint32_t depth_test, depth_op, depth_write, stencil, stencil_func, stencil_fail, stencil_zfail, stencil_pass;
} PipeKey;
typedef struct { PipeKey key; VkPipeline pipe; } Pipe;
static Pipe *pipes;
static size_t npipes, pipes_cap;

static const uint32_t tev_base[6] = { 0x0C0, 0x0C8, 0x0D0, 0x0D8, 0x0F0, 0x0F8 };

/* PICA encodings -> Vulkan (gpu/draw.c's blend_factor, blend_eq, test, logic_op). */
static const VkBlendFactor blend_factors[15] = {
    VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
    VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA,
    VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
    VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR, VK_BLEND_FACTOR_CONSTANT_ALPHA,
    VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
};
static const VkBlendOp blend_ops[8] = {
    VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX,
    VK_BLEND_OP_ADD, VK_BLEND_OP_ADD, VK_BLEND_OP_ADD,
};
static const VkCompareOp compare_ops[8] = {
    VK_COMPARE_OP_NEVER, VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_NOT_EQUAL,
    VK_COMPARE_OP_LESS, VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_GREATER, VK_COMPARE_OP_GREATER_OR_EQUAL,
};
static const VkLogicOp logic_ops[16] = {
    VK_LOGIC_OP_CLEAR, VK_LOGIC_OP_AND, VK_LOGIC_OP_AND_REVERSE, VK_LOGIC_OP_COPY, VK_LOGIC_OP_SET,
    VK_LOGIC_OP_COPY_INVERTED, VK_LOGIC_OP_NO_OP, VK_LOGIC_OP_INVERT, VK_LOGIC_OP_NAND, VK_LOGIC_OP_OR,
    VK_LOGIC_OP_NOR, VK_LOGIC_OP_XOR, VK_LOGIC_OP_EQUIVALENT, VK_LOGIC_OP_AND_INVERTED, VK_LOGIC_OP_OR_REVERSE,
    VK_LOGIC_OP_OR_INVERTED,
};

static void check(VkResult r, const char *what)
{
    if (r != VK_SUCCESS)
        gpu_fatal("Vulkan: %s failed (VkResult %d)", what, (int)r);
}

/* ---------------------------------------------------------------- setup */

static PFN_vkGetInstanceProcAddr load_loader(void)
{
#ifdef _WIN32
    HMODULE m = LoadLibraryA("vulkan-1.dll");
    return m ? (PFN_vkGetInstanceProcAddr)(void (*)(void))GetProcAddress(m, "vkGetInstanceProcAddr") : NULL;
#else
    void *m = dlopen("libvulkan.so.1", RTLD_NOW);
    return m ? (PFN_vkGetInstanceProcAddr)dlsym(m, "vkGetInstanceProcAddr") : NULL;
#endif
}

static int rank(const VkPhysicalDeviceProperties *p)
{
    switch (p->deviceType) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 4;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:            return 1;
    default:                                     return 0;
    }
}

static void pick_gpu(void)
{
    VkPhysicalDevice devs[16];
    uint32_t n = 16;
    vkEnumeratePhysicalDevices(instance, &n, devs);
    const char *want = getenv("GPU_DEVICE");
    int best = -1, best_rank = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        VkQueueFamilyProperties fam[16];
        uint32_t nf = 16, f;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nf, fam);
        for (f = 0; f < nf && !(fam[f].queueFlags & VK_QUEUE_GRAPHICS_BIT); f++)
            ;
        if (f == nf)
            continue;
        int r = want ? (strstr(p.deviceName, want) != NULL) : rank(&p);
        if (r > best_rank) {
            best = (int)i;
            best_rank = r;
            family = f;
        }
    }
    if (best < 0 || (want && best_rank == 0))
        gpu_fatal("GPU_BACKEND=vulkan: no Vulkan device%s%s", want ? " named like " : "", want ? want : "");
    gpu = devs[best];
}

static int find_memory_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties p;
    vkGetPhysicalDeviceMemoryProperties(gpu, &p);
    for (uint32_t i = 0; i < p.memoryTypeCount; i++)
        if ((bits >> i & 1) && (p.memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    return -1;
}

static int memory_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    int i = find_memory_type(bits, want);
    if (i < 0)
        gpu_fatal("Vulkan: no memory type with properties %x", (unsigned)want);
    return i;
}

static VkDeviceMemory allocate(VkMemoryRequirements req, VkMemoryPropertyFlags want)
{
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                .memoryTypeIndex = (uint32_t)memory_type(req.memoryTypeBits, want) };
    VkDeviceMemory m;
    check(vkAllocateMemory(device, &ai, NULL, &m), "vkAllocateMemory");
    return m;
}

/* Make sure `b` holds at least `need` bytes (host-visible, mapped). Only
   called between batches, when the GPU is idle. */
static void ensure(Buffer *b, VkDeviceSize need)
{
    if (need <= b->size)
        return;
    if (b->buf) {
        vkDestroyBuffer(device, b->buf, NULL);
        vkFreeMemory(device, b->mem, NULL);
    }
    b->size = need > 2 * b->size ? need : 2 * b->size;
    if (b->size < 65536)
        b->size = 65536;
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = b->size, .usage = b->usage };
    check(vkCreateBuffer(device, &bi, NULL, &b->buf), "vkCreateBuffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, b->buf, &req);
    /* The CPU reads read-back buffers: uncached memory makes that ~10x slower (NVIDIA) */
    VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if ((b->usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT)
        && find_memory_type(req.memoryTypeBits, want | VK_MEMORY_PROPERTY_HOST_CACHED_BIT) >= 0)
        want |= VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    b->mem = allocate(req, want);
    vkBindBufferMemory(device, b->buf, b->mem, 0);
    void *p;
    check(vkMapMemory(device, b->mem, 0, b->size, 0, &p), "vkMapMemory");
    b->ptr = p;
}

static VkShaderModule shader(const uint32_t *code, size_t size)
{
    VkShaderModuleCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code };
    VkShaderModule m;
    check(vkCreateShaderModule(device, &si, NULL, &m), "vkCreateShaderModule");
    return m;
}

static void make_pass(void)
{
    VkAttachmentDescription at[2] = {
        { .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
          .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL },
        { .format = DS_FORMAT, .samples = VK_SAMPLE_COUNT_1_BIT,
          .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
          .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL },
    };
    VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_GENERAL }, dref = { 1, VK_IMAGE_LAYOUT_GENERAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1,
                                 .pColorAttachments = &cref, .pDepthStencilAttachment = &dref };
    VkRenderPassCreateInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 2,
                                  .pAttachments = at, .subpassCount = 1, .pSubpasses = &sub };
    check(vkCreateRenderPass(device, &ri, NULL, &pass), "vkCreateRenderPass");
}

static void make_layout(void)
{
    VkDescriptorSetLayoutBinding b[3] = {
        { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
    };
    VkDescriptorSetLayoutCreateInfo li = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .bindingCount = 3, .pBindings = b };
    check(vkCreateDescriptorSetLayout(device, &li, NULL, &set_layout), "vkCreateDescriptorSetLayout");
    VkPipelineLayoutCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                      .setLayoutCount = 1, .pSetLayouts = &set_layout };
    check(vkCreatePipelineLayout(device, &pi, NULL, &layout), "vkCreatePipelineLayout");

    VkDescriptorPoolSize ps[3] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NSETS },
                                   { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * NSETS },
                                   { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NSETS } };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = NSETS,
                                       .poolSizeCount = 3, .pPoolSizes = ps };
    check(vkCreateDescriptorPool(device, &dpi, NULL, &dpool), "vkCreateDescriptorPool");
}

static void report(void)
{
    fprintf(stderr, "gpu: %llu batches drawn with Vulkan, in %llu submissions; %llu target uploads\n",
            (unsigned long long)batches, (unsigned long long)submits, (unsigned long long)uploads);
    for (int i = 0; i < NWHY && refusals[i].why; i++)
        fprintf(stderr, "gpu: %llu batches left to software for %s\n", (unsigned long long)refusals[i].n,
                refusals[i].why);
}

static void init(void)
{
    vkGetInstanceProcAddr = load_loader();
    if (!vkGetInstanceProcAddr)
        gpu_fatal("GPU_BACKEND=vulkan: no Vulkan loader (libvulkan.so.1 / vulkan-1.dll)");
    vkCreateInstance = (PFN_vkCreateInstance)vkGetInstanceProcAddr(NULL, "vkCreateInstance");
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "any3ds",
                              .apiVersion = VK_API_VERSION_1_0 };
    VkInstanceCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    check(vkCreateInstance(&ii, NULL, &instance), "vkCreateInstance");
#define LOAD_INSTANCE(name) name = (PFN_##name)vkGetInstanceProcAddr(instance, #name);
    INSTANCE_FNS(LOAD_INSTANCE)
    pick_gpu();

    VkPhysicalDeviceFeatures have, use = { 0 };
    vkGetPhysicalDeviceFeatures(gpu, &have);
    has_logic_op = have.logicOp;
    use.logicOp = have.logicOp;
    float prio = 1;
    VkDeviceQueueCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family,
                                   .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo di = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qi, .pEnabledFeatures = &use };
    check(vkCreateDevice(gpu, &di, NULL, &device), "vkCreateDevice");
#define LOAD_DEVICE(name) name = (PFN_##name)vkGetDeviceProcAddr(device, #name);
    DEVICE_FNS(LOAD_DEVICE)
    vkGetDeviceQueue(device, family, 0, &queue);

    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(gpu, DS_FORMAT, &fp);
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if ((fp.optimalTilingFeatures & need) != need)
        gpu_fatal("GPU_BACKEND=vulkan: this GPU has no D32_SFLOAT_S8_UINT depth buffers");

    VkCommandPoolCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family };
    check(vkCreateCommandPool(device, &ci, NULL, &pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    check(vkAllocateCommandBuffers(device, &ai, &cmd), "vkAllocateCommandBuffers");
    check(vkAllocateCommandBuffers(device, &ai, &tcmd), "vkAllocateCommandBuffers");
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    check(vkCreateFence(device, &fi, NULL, &done), "vkCreateFence");
    check(vkCreateFence(device, &fi, NULL, &tdone), "vkCreateFence");
    /* The shader reads texels with texelFetch: the sampler only has to exist. */
    VkSamplerCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                                .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                .maxLod = 16 };
    check(vkCreateSampler(device, &smi, NULL, &sampler), "vkCreateSampler");

    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(gpu, &p);
    ubo_align = p.limits.minUniformBufferOffsetAlignment;
    sbo_align = p.limits.minStorageBufferOffsetAlignment;
    ensure(&ubo, 256 * NSETS);
    ensure(&tables, TAB_WORDS * 4 * 16);
    ensure(&vbo, 4 << 20);
    ensure(&up, 8 << 20);

    tile_tables();
    make_pass();
    make_layout();
    make_dummy();
    vert = shader(draw_vert_spv, sizeof draw_vert_spv);
    frag = shader(draw_frag_spv, sizeof draw_frag_spv);

    fprintf(stderr, "gpu: drawing with Vulkan on %s\n", p.deviceName);
    atexit(report);
}

int vkr_enabled(void)
{
    if (state < 0) {
        const char *e = getenv("GPU_BACKEND");
        state = 0;
        if (e && strcmp(e, "vulkan") == 0) {
            init();
            state = 1;
        } else if (e && strcmp(e, "software") != 0) {
            gpu_fatal("GPU_BACKEND=%s: the backends are software and vulkan", e);
        }
    }
    return state;
}

/* ---------------------------------------------------------------- surfaces */

static void free_surface(Surface *s)
{
    vkDestroyFramebuffer(device, s->fb, NULL);
    vkDestroyImageView(device, s->cview, NULL);
    vkDestroyImageView(device, s->dview, NULL);
    vkDestroyImage(device, s->cimg, NULL);
    vkDestroyImage(device, s->ds, NULL);
    vkFreeMemory(device, s->cmem, NULL);
    vkFreeMemory(device, s->dmem, NULL);
    memset(s, 0, sizeof *s);
}

static size_t color_bytes(const Surface *s)
{
    return s->color ? (size_t)s->w * s->h * (size_t)pixel_size(s->color_fmt) : 0;
}

static uint32_t depth_size(int fmt) { return fmt == 0 ? 2 : fmt == 2 ? 3 : 4; }

static size_t depth_bytes(const Surface *s)
{
    return s->depth ? (size_t)s->w * s->h * depth_size(s->depth_fmt) : 0;
}

static uint64_t surface_hash(const Surface *s)
{
    uint64_t t0 = gpu_now_ns();
    uint64_t h = hash_bytes(s->color, color_bytes(s), 0xCBF29CE484222325ull);
    h = hash_bytes(s->depth, depth_bytes(s), h);
    vt[VKT_HASH] += gpu_now_ns() - t0;
    return h;
}

static int overlap(const uint8_t *a, size_t an, const uint8_t *b, size_t bn)
{
    return an && bn && a < b + bn && b < a + an;
}

/* Whether surface `s` mirrors any of the n bytes at p. */
static int surface_covers(const Surface *s, const uint8_t *p, size_t n)
{
    return overlap(s->color, color_bytes(s), p, n) || overlap(s->depth, depth_bytes(s), p, n);
}

/* Read back the surfaces the GPU drew on before anyone reads these bytes. */
static void sync_if_drawn(const uint8_t *p, size_t n)
{
    for (int i = 0; i < NSURF; i++)
        if ((surf[i].dirty_color || surf[i].dirty_depth) && surface_covers(&surf[i], p, n)) {
            vkr_sync();
            return;
        }
}

static void make_image(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                       VkImage *img, VkDeviceMemory *mem, VkImageView *view)
{
    VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                             .format = format, .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1,
                             .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .usage = usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    check(vkCreateImage(device, &ii, NULL, img), "vkCreateImage");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, *img, &req);
    *mem = allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkBindImageMemory(device, *img, *mem, 0);
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = *img,
                                 .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
                                 .subresourceRange = { aspect, 0, 1, 0, 1 } };
    check(vkCreateImageView(device, &vi, NULL, view), "vkCreateImageView");
}

static Surface *get_surface(const VkrTarget *t)
{
    Surface *s = NULL;
    for (int i = 0; i < NSURF && !s; i++)
        if (surf[i].fb && surf[i].w == t->width && surf[i].h == t->height && surf[i].color == t->color &&
            surf[i].depth == t->depth && surf[i].color_fmt == t->color_fmt && surf[i].depth_fmt == t->depth_fmt)
            s = &surf[i];
    if (!s) {
        s = &surf[0];                   /* a free slot, or else the least recently used */
        for (int i = 1; i < NSURF; i++)
            if (surf[i].used < s->used)
                s = &surf[i];
        if (s->fb) {
            if (recording)              /* its batches, and what they drew, first */
                vkr_sync();
            free_surface(s);
        }
        s->w = t->width;
        s->h = t->height;
        s->color = t->color;
        s->depth = t->depth;
        s->color_fmt = t->color_fmt;
        s->depth_fmt = t->depth_fmt;
        make_image(s->w, s->h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                   &s->cimg, &s->cmem, &s->cview);
        make_image(s->w, s->h, DS_FORMAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                   VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, &s->ds, &s->dmem, &s->dview);
        VkImageView views[2] = { s->cview, s->dview };
        VkFramebufferCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = pass,
                                       .attachmentCount = 2, .pAttachments = views, .width = s->w, .height = s->h,
                                       .layers = 1 };
        check(vkCreateFramebuffer(device, &fi, NULL, &s->fb), "vkCreateFramebuffer");
    }
    s->used = ++clock_;
    return s;
}

/* ---------------------------------------------------------------- textures */

/* Texture units 0-2's textures, decoded to RGBA8 by texture.c's decoder:
   rows in memory order, cube faces as layers, and every mip level
   texture.c's sampling can reach. Kept by address, size, format and a hash
   of the bytes, so a texture the game rewrites is decoded again. */
typedef struct {
    uint32_t addr[6], width, height, format, faces, levels;
    uint64_t hash;
} TexKey;
typedef struct {
    TexKey key;
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;
    uint64_t used, epoch;               /* epoch: the last submission that reads it */
    uint64_t checked;                   /* mem_epoch when its bytes were last hashed */
} Texture;
#define NTEX 512
static Texture textures[NTEX];

static uint64_t hash_bytes(const uint8_t *p, size_t n, uint64_t h)
{
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    for (; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

/* The levels texture.c's tex_sample can read: up to the unit's largest
   level, stopping at 8 texels (its loop), plus the next for trilinear. */
static uint32_t tex_levels(const TexUnit *t)
{
    uint32_t max_level = t->lod >> 16 & 15, n = 1, w = t->width, h = t->height;
    for (uint32_t l = 0; l < max_level && w > 8 && h > 8; l++, n++) {
        w /= 2;
        h /= 2;
    }
    return n;
}

static void upload_texture(Texture *x, const TexUnit *t, const uint8_t *const *data)
{
    const TexKey *k = &x->key;
    VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                             .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { k->width, k->height, 1 },
                             .mipLevels = k->levels, .arrayLayers = k->faces, .samples = VK_SAMPLE_COUNT_1_BIT,
                             .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    check(vkCreateImage(device, &ii, NULL, &x->img), "vkCreateImage");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, x->img, &req);
    x->mem = allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkBindImageMemory(device, x->img, x->mem, 0);
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = x->img,
                                 .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY, .format = VK_FORMAT_R8G8B8A8_UNORM,
                                 .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, k->levels, 0, k->faces } };
    check(vkCreateImageView(device, &vi, NULL, &x->view), "vkCreateImageView");

    /* decode every face and level into the staging buffer */
    VkDeviceSize total = 0;
    for (uint32_t l = 0; l < k->levels; l++)
        total += 4 * (VkDeviceSize)(k->width >> l) * (k->height >> l) * k->faces;
    ensure(&tup, total);
    VkBufferImageCopy regions[6 * 16];
    uint32_t nreg = 0;
    VkDeviceSize at = 0;
    for (uint32_t f = 0; f < k->faces; f++) {
        const uint8_t *level = data[f];
        for (uint32_t l = 0; l < k->levels; l++) {
            uint32_t w = k->width >> l, h = k->height >> l;
            Rgba *out = (Rgba *)(tup.ptr + at);
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t xx = 0; xx < w; xx++)
                    out[y * w + xx] = tex_decode(level, t->format, w, xx, y);
            regions[nreg++] = (VkBufferImageCopy){ .bufferOffset = at,
                .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, l, f, 1 }, .imageExtent = { w, h, 1 } };
            at += 4 * (VkDeviceSize)w * h;
            level += tex_level_size(t->format, w, h);
        }
    }

    check(vkResetFences(device, 1, &tdone), "vkResetFences");
    vkResetCommandBuffer(tcmd, 0);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(tcmd, &bi);
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = x->img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, k->levels, 0, k->faces } };
    vkCmdPipelineBarrier(tcmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    vkCmdCopyBufferToImage(tcmd, tup.buf, x->img, VK_IMAGE_LAYOUT_GENERAL, nreg, regions);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_SHADER_READ_BIT };
    vkCmdPipelineBarrier(tcmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb,
                         0, NULL, 0, NULL);
    vkEndCommandBuffer(tcmd);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &tcmd };
    check(vkQueueSubmit(queue, 1, &si, tdone), "vkQueueSubmit");
    check(vkWaitForFences(device, 1, &tdone, VK_TRUE, UINT64_MAX), "vkWaitForFences");
}

/* Texture unit `unit`'s texture on the GPU, decoded if it isn't yet. */
static Texture *get_texture_(int unit);
static Texture *get_texture(int unit)
{
    uint64_t t0 = gpu_now_ns();
    Texture *x = get_texture_(unit);
    vt[VKT_TEXTURE] += gpu_now_ns() - t0;
    return x;
}

static Texture *get_texture_(int unit)
{
    TexUnit t;
    tex_unit_read(unit, &t);
    TexKey k;
    memset(&k, 0, sizeof k);
    k.width = t.width;
    k.height = t.height;
    k.format = t.format;
    k.faces = t.type == 1 ? 6 : 1;
    k.levels = tex_levels(&t);
    const uint8_t *data[6];
    uint32_t bytes = 0, w = t.width, h = t.height;
    for (uint32_t l = 0; l < k.levels; l++, w /= 2, h /= 2)
        bytes += tex_level_size(t.format, w, h);
    for (uint32_t f = 0; f < k.faces; f++) {
        k.addr[f] = t.addr[f];
        uint32_t v = gpu_phys_to_virt(t.addr[f]);
        if (!v)
            gpu_fatal("texture at physical %08" PRIx32 " is unmapped", t.addr[f]);
        data[f] = gpu_mem(v, bytes);
        sync_if_drawn(data[f], bytes);      /* rendering to a texture */
    }

    /* Memory only changes at a vkr_sync: a texture whose bytes were hashed
       since is still the same. */
    static Texture *recent[256];
    uint32_t hint = (k.addr[0] >> 3 ^ k.width * 31 ^ k.height * 131 ^ k.format * 7 ^ k.levels) & 255;
    Texture *r = recent[hint];
    if (r && r->img && r->checked == mem_epoch) {
        TexKey same = r->key;
        same.hash = 0;
        if (memcmp(&same, &k, sizeof k) == 0) {
            r->used = ++clock_;
            return r;
        }
    }

    uint64_t hash = 0xCBF29CE484222325ull;
    for (uint32_t f = 0; f < k.faces; f++)
        hash = hash_bytes(data[f], bytes, hash);
    k.hash = hash;

    Texture *x = NULL, *lru = &textures[0];
    for (int i = 0; i < NTEX && !x; i++) {
        if (textures[i].img && memcmp(&textures[i].key, &k, sizeof k) == 0)
            x = &textures[i];
        else if (textures[i].used < lru->used)
            lru = &textures[i];
    }
    if (!x) {
        x = lru;
        if (x->img && x->epoch == epoch && recording)
            vkr_sync();                     /* recorded batches still read it */
        if (x->img) {
            vkDestroyImageView(device, x->view, NULL);
            vkDestroyImage(device, x->img, NULL);
            vkFreeMemory(device, x->mem, NULL);
        }
        x->key = k;
        upload_texture(x, &t, data);
    }
    x->used = ++clock_;
    x->checked = mem_epoch;
    recent[hint] = x;
    return x;
}

/* What unread units are bound to: 8x8 black, never matched (it has no
   address) and never evicted. */
static void make_dummy(void)
{
    Texture *dummy = &textures[0];
    static const uint8_t zeros[4 * 8 * 8 * 2];
    const uint8_t *zp = zeros;
    TexUnit zt = { .width = 8, .height = 8, .format = 0 };
    dummy->key = (TexKey){ .width = 8, .height = 8, .faces = 1, .levels = 1, .hash = 1 };
    upload_texture(dummy, &zt, &zp);
    dummy_view = dummy->view;
    dummy->used = UINT64_MAX;               /* never evicted */
}

/* Which combiner sources the six stages read: bit i for source i. */
static int sources_read(void)
{
    int used = 0;
    for (int i = 0; i < 6; i++) {
        const uint32_t *r = &pica.reg[tev_base[i]];
        uint32_t rgb_mode = r[2] & 0xF, alpha_mode = r[2] >> 16 & 0xF;
        int nr = rgb_mode == 0 ? 1 : (rgb_mode == 4 || rgb_mode == 8 || rgb_mode == 9) ? 3 : 2;
        int na = alpha_mode == 0 ? 1 : (alpha_mode == 4 || alpha_mode == 8 || alpha_mode == 9) ? 3 : 2;
        for (int k = 0; k < 3; k++) {
            uint32_t rs = r[0] >> (4 * k) & 0xF, as = r[0] >> (16 + 4 * k) & 0xF;
            if (k < nr)
                used |= 1 << rs;
            if (k < na)
                used |= 1 << as;
        }
    }
    return used;
}

/* Fragment lighting is on and a combiner reads it. */
static int lighting_read(void)
{
    return (pica.reg[REG_LIGHTING_ENABLE0] & 1) && (sources_read() & 6);
}

/* The texture units the shader samples (bits 0-2): those the combiners read,
   and a normal map's. */
static int units_read(void)
{
    int used = sources_read() >> 3 & 7;
    uint32_t c0 = pica.reg[0x1C3];
    if (lighting_read() && (c0 >> 28 & 3) == 1)
        used |= 1 << (c0 >> 22 & 3);
    return used;
}

/* A unit that is off, or on with no size: gpu/draw.c's frag_texture reads
   unset_color() from it (zero, or white with GPU_UNSET=white). */
static int unit_unset(int unit)
{
    static const uint32_t dim_reg[3] = { 0x082, 0x092, 0x09A };
    uint32_t dim = pica.reg[dim_reg[unit]];
    return !(pica.reg[REG_TEXUNIT_CONFIG] >> unit & 1) || !(dim & 0x7FF) || !(dim >> 16 & 0x7FF);
}

static uint32_t unset_color(void)
{
    const char *e = getenv("GPU_UNSET");
    return e && strcmp(e, "white") == 0 ? 0xFFFFFFFFu : 0;
}

/* A unit the GPU path can sample: what texture.c's tex_unit_read and
   frag_texture accept without stopping or reading zero. */
static int unit_ok(int unit)
{
    static const uint32_t dim_reg[3] = { 0x082, 0x092, 0x09A }, param_reg[3] = { 0x083, 0x093, 0x09B },
                          type_reg[3] = { 0x08E, 0x096, 0x09E }, addr_reg[3] = { 0x085, 0x095, 0x09D };
    if (!(pica.reg[REG_TEXUNIT_CONFIG] >> unit & 1))
        return 0;
    uint32_t dim = pica.reg[dim_reg[unit]], h = dim & 0x7FF, w = dim >> 16 & 0x7FF, param = pica.reg[param_reg[unit]];
    uint32_t type = unit == 0 ? param >> 28 & 7 : 0;
    if (!w || !h || w % 8 || h % 8 || (pica.reg[type_reg[unit]] & 0xF) > 13 || (type != 0 && type != 1 && type != 3))
        return 0;
    if ((param >> 12 & 7) > 3 || (param >> 8 & 7) > 3)
        return 0;
    if (!gpu_phys_to_virt(pica.reg[addr_reg[unit]] << 3))
        return 0;
    return 1;
}

/* ---------------------------------------------------------------- pipelines */

static VkPipeline make_pipeline(const PipeKey *k)
{
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = vert, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = frag, .pName = "main" },
    };
    VkVertexInputBindingDescription bind = { 0, sizeof(VkrVertex), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attrs[1 + VKR_SLOTS / 4];
    for (uint32_t i = 0; i < 1 + VKR_SLOTS / 4; i++)
        attrs[i] = (VkVertexInputAttributeDescription){ i, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
                                                        i ? (uint32_t)offsetof(VkrVertex, a) + 16 * (i - 1) : 0 };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &bind,
        .vertexAttributeDescriptionCount = 1 + VKR_SLOTS / 4, .pVertexAttributeDescriptions = attrs };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    /* Culling happens in gpu/draw.c, by the same test as the software path. */
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1 };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkStencilOpState so = { (VkStencilOp)k->stencil_fail, (VkStencilOp)k->stencil_pass, (VkStencilOp)k->stencil_zfail,
                            compare_ops[k->stencil_func], 0, 0, 0 };
    VkPipelineDepthStencilStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = k->depth_test, .depthWriteEnable = k->depth_write, .depthCompareOp = compare_ops[k->depth_op],
        .stencilTestEnable = k->stencil, .front = so, .back = so, .maxDepthBounds = 1 };
    VkPipelineColorBlendAttachmentState ba = { .blendEnable = k->blend,
        .srcColorBlendFactor = blend_factors[k->rgb_src], .dstColorBlendFactor = blend_factors[k->rgb_dst],
        .colorBlendOp = blend_ops[k->rgb_eq], .srcAlphaBlendFactor = blend_factors[k->alpha_src],
        .dstAlphaBlendFactor = blend_factors[k->alpha_dst], .alphaBlendOp = blend_ops[k->alpha_eq],
        .colorWriteMask = k->mask };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = k->logic, .logicOp = logic_ops[k->logic_op], .attachmentCount = 1, .pAttachments = &ba };
    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                             VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                             VK_DYNAMIC_STATE_STENCIL_REFERENCE };
    VkPipelineDynamicStateCreateInfo dy = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = sizeof dyn / sizeof dyn[0], .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2,
        .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp,
        .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb,
        .pDynamicState = &dy, .layout = layout, .renderPass = pass };
    VkPipeline p;
    check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, NULL, &p), "vkCreateGraphicsPipelines");
    return p;
}

static VkPipeline get_pipeline(const PipeKey *k)
{
    for (size_t i = 0; i < npipes; i++)
        if (memcmp(&pipes[i].key, k, sizeof *k) == 0)
            return pipes[i].pipe;
    if (npipes == pipes_cap) {
        pipes_cap = pipes_cap ? 2 * pipes_cap : 64;
        pipes = realloc(pipes, pipes_cap * sizeof *pipes);
        if (!pipes)
            gpu_fatal("out of memory for Vulkan pipelines");
    }
    pipes[npipes].key = *k;
    pipes[npipes].pipe = make_pipeline(k);
    return pipes[npipes++].pipe;
}

/* ---------------------------------------------------------------- drawing */

/* Why a combiner source keeps a batch on the software path, or NULL. */
/* What lighting.c stops on: none of it reaches the shader. */
static const char *lighting_refused(void)
{
    uint32_t c0 = pica.reg[0x1C3], c1 = pica.reg[0x1C4], sel = pica.reg[0x1D1];
    if (c0 & 1)
        return "shadowed fragment lighting";
    uint32_t bump = c0 >> 28 & 3;
    if (bump > 1)
        return "tangent-map lighting";
    if (bump == 1 && ((c0 >> 22 & 3) > 2 || (!unit_unset((int)(c0 >> 22 & 3)) && !unit_ok((int)(c0 >> 22 & 3)))))
        return "a shadow texture or an unmapped one";
    static const int off_bit[7] = { 16, 17, -1, 19, 20, 21, 22 };     /* tables D0 D1 SP FR RB RG RR */
    int nlights = (int)(pica.reg[0x1C2] & 7) + 1, spot = 0;
    for (int k = 0; k < nlights; k++) {
        int id = (int)(pica.reg[0x1D9] >> (4 * k) & 7);
        if (pica.reg[0x140 + 0x10 * id + 9] & 0xC)
            return "lighting geometric factors";
        spot |= !(c1 >> (8 + id) & 1);
    }
    for (int which = 0; which < 7; which++)
        if ((which == 2 ? spot : !(c1 >> off_bit[which] & 1)) && (sel >> (4 * which) & 7) > 4)
            return "lighting's cos(phi) input";
    return NULL;
}

static const char *src_refused(uint32_t src)
{
    switch (src) {
    case 0: case 13: case 14: case 15:
        return NULL;                    /* the vertex colour, the buffer, the constant, the previous stage */
    case 1: case 2:                     /* with lighting off, they read unset_color() */
        return pica.reg[REG_LIGHTING_ENABLE0] & 1 ? lighting_refused() : NULL;
    case 3: case 4: case 5:
        return unit_unset((int)src - 3) || unit_ok((int)src - 3) ? NULL : "a shadow texture or an unmapped one";
    case 6:
        return "the procedural texture";
    default:
        return "an unknown combiner source";
    }
}

/* Why the current state keeps a batch on the software path, or NULL. */
static const char *refused(void)
{
    for (int i = 0; i < 6; i++) {
        const uint32_t *r = &pica.reg[tev_base[i]];
        uint32_t rgb_mode = r[2] & 0xF, alpha_mode = r[2] >> 16 & 0xF;
        if (rgb_mode > 9 || alpha_mode > 9 || alpha_mode == 6 || alpha_mode == 7)
            return "an unknown combiner mode";     /* (the software path names it) */
        int nr = rgb_mode == 0 ? 1 : (rgb_mode == 4 || rgb_mode == 8 || rgb_mode == 9) ? 3 : 2;
        int na = alpha_mode == 0 ? 1 : (alpha_mode == 4 || alpha_mode == 8 || alpha_mode == 9) ? 3 : 2;
        for (int k = 0; k < nr; k++) {
            uint32_t op = r[1] >> (4 * k) & 0xF;
            const char *why = src_refused(r[0] >> (4 * k) & 0xF);
            if (why)
                return why;
            if (op >> 1 != 0 && op >> 1 != 1 && op >> 1 != 2 && op >> 1 != 4 && op >> 1 != 6)
                return "an unknown combiner operand";
        }
        for (int k = 0; k < na; k++) {
            const char *why = src_refused(r[0] >> (16 + 4 * k) & 0xF);
            if (why)
                return why;
        }
    }
    uint32_t fog = pica.reg[REG_TEXENV_UPDATE_BUFFER] & 7;
    if (fog && fog != 5)
        return "gas";
    if (pica.reg[REG_COLOR_OPERATION] & 3)
        return "gas or shadow rendering";
    if (pica.reg[REG_FRAGOP_CLIP] & 1)
        return "a user clip plane";
    if ((pica.reg[REG_SCISSOR_MODE] & 3) == 1)
        return "scissoring the outside of a box";
    if (pica.reg[REG_COLOR_OPERATION] >> 8 & 1) {
        uint32_t bf = pica.reg[REG_BLEND_FUNC];
        for (int sh = 16; sh < 32; sh += 4)
            if ((bf >> sh & 0xF) > 14)
                return "an unknown blend factor";
        /* Vulkan's min and max ignore the blend factors; the PICA's apply them. */
        uint32_t eq = bf & 7, aeq = bf >> 8 & 7;
        if (((eq == 3 || eq == 4) && (bf >> 16 & 0xFF) != 0x11) || ((aeq == 3 || aeq == 4) && (bf >> 24) != 0x11))
            return "min/max blending with blend factors";
    } else if (!has_logic_op && (pica.reg[REG_LOGIC_OP] & 0xF) != 3) {
        return "a logic op (this GPU has none)";
    }
    return NULL;
}

int vkr_can_draw(void)
{
    const char *why = refused();
    if (!why)
        return 1;
    for (int i = 0; i < NWHY; i++)
        if (refusals[i].why == why || !refusals[i].why) {
            refusals[i].why = why;
            refusals[i].n++;
            break;
        }
    return 0;
}

static void barrier(VkPipelineStageFlags src, VkAccessFlags src_access, VkPipelineStageFlags dst, VkAccessFlags dst_access)
{
    VkMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = src_access,
                          .dstAccessMask = dst_access };
    vkCmdPipelineBarrier(cmd, src, dst, 0, 1, &b, 0, NULL, 0, NULL);
}

#define ATTACH_STAGES (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | \
                       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT)
#define ATTACH_RW (VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | \
                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)

/* Pixel k of an 8x8 tile in memory is at (tile_x[k], tile_y[k]) in it. */
static uint8_t tile_x[64], tile_y[64];

static void tile_tables(void)
{
    for (uint32_t y = 0; y < 8; y++)
        for (uint32_t x = 0; x < 8; x++) {
            uint32_t k = tile_index(x, y, 8);
            tile_x[k] = (uint8_t)x;
            tile_y[k] = (uint8_t)y;
        }
}

/* The common case, a whole number of tiles: memory in order, pixel by
   pixel, with RGBA8 colour (byte-swapped words). Same results as below. */
static int fast_layout(const Surface *s)
{
    return s->w % 8 == 0 && s->h % 8 == 0 && (!s->color || s->color_fmt == PIXEL_RGBA8);
}

static void upload_fast(const Surface *s, uint8_t *p)
{
    uint32_t w = s->w, h = s->h, npix = w * h, bits = s->depth_fmt == 0 ? 16 : 24;
    uint32_t *color = (uint32_t *)p;
    float *depth = (float *)(p + 4 * (size_t)npix);
    uint8_t *stencil = p + 8 * (size_t)npix;
    uint32_t dsize = depth_size(s->depth_fmt), dmax = (1u << bits) - 1;
    float scale = ldexpf(1, -(int)bits);
    uint32_t idx = 0;
    for (uint32_t ty = 0; ty < h; ty += 8)
        for (uint32_t tx = 0; tx < w; tx += 8)
            for (uint32_t k = 0; k < 64; k++, idx++) {
                uint32_t i = (ty + tile_y[k]) * w + tx + tile_x[k];
                uint32_t c = 0;
                if (s->color) {
                    memcpy(&c, s->color + 4 * (size_t)idx, 4);
                    c = __builtin_bswap32(c);       /* memory a, b, g, r -> r, g, b, a */
                }
                color[i] = c;
                uint32_t raw = 0;
                if (s->depth)
                    memcpy(&raw, s->depth + (size_t)idx * dsize, dsize);
                depth[i] = (float)(raw & dmax) * scale;
                stencil[i] = s->depth_fmt == 3 ? (uint8_t)(raw >> 24) : 0;
            }
}

static void download_fast(const Surface *s, const uint8_t *p)
{
    uint32_t w = s->w, h = s->h, npix = w * h, bits = s->depth_fmt == 0 ? 16 : 24;
    int color = s->dirty_color && s->color, depth = s->dirty_depth && s->depth;
    const uint32_t *cv = (const uint32_t *)p;
    const float *dv = (const float *)(p + 4 * (size_t)npix);
    const uint8_t *sv = p + 8 * (size_t)npix;
    uint32_t dsize = depth_size(s->depth_fmt);
    float scale = ldexpf(1, (int)bits);
    uint32_t idx = 0;
    for (uint32_t ty = 0; ty < h; ty += 8)
        for (uint32_t tx = 0; tx < w; tx += 8)
            for (uint32_t k = 0; k < 64; k++, idx++) {
                uint32_t i = (ty + tile_y[k]) * w + tx + tile_x[k];
                if (color) {
                    uint32_t c = __builtin_bswap32(cv[i]);
                    memcpy(s->color + 4 * (size_t)idx, &c, 4);
                }
                if (depth) {
                    uint32_t z = (uint32_t)(dv[i] * scale);
                    uint8_t *dp = s->depth + (size_t)idx * dsize;
                    if (s->depth_fmt == 3) {
                        z = (z & 0xFFFFFF) | (uint32_t)sv[i] << 24;
                        memcpy(dp, &z, 4);
                    } else {
                        memcpy(dp, &z, dsize == 2 ? 2 : 3);
                    }
                }
            }
}

/* 3DS memory -> an upload area: colour as RGBA8, then depth as floats,
   then stencil bytes, each in rows from the target's first memory row. */
static void upload(const Surface *s, uint8_t *p)
{
    if (fast_layout(s)) {
        upload_fast(s, p);
        return;
    }
    uint32_t w = s->w, h = s->h, npix = w * h, bits = s->depth_fmt == 0 ? 16 : 24;
    float *depth = (float *)(p + 4 * (size_t)npix);
    uint8_t *stencil = p + 8 * (size_t)npix;
    uint32_t csize = s->color ? (uint32_t)pixel_size(s->color_fmt) : 0, dsize = depth_size(s->depth_fmt);
    uint32_t dmax = (1u << bits) - 1;
    float scale = ldexpf(1, -(int)bits);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint32_t idx = tile_index(x, y, w), i = y * w + x;
            Rgba c = s->color ? color_decode(s->color + idx * csize, s->color_fmt) : (Rgba){ 0, 0, 0, 0 };
            memcpy(p + 4 * (size_t)i, &c, 4);
            uint32_t raw = 0;
            if (s->depth)
                memcpy(&raw, s->depth + idx * dsize, dsize);
            depth[i] = (float)(raw & dmax) * scale;
            stencil[i] = s->depth_fmt == 3 ? (uint8_t)(raw >> 24) : 0;
        }
}

/* A read-back area -> 3DS memory, for what the GPU drew on. */
static void download(const Surface *s, const uint8_t *p)
{
    if (fast_layout(s)) {
        download_fast(s, p);
        return;
    }
    uint32_t w = s->w, h = s->h, npix = w * h, bits = s->depth_fmt == 0 ? 16 : 24;
    int color = s->dirty_color && s->color, depth = s->dirty_depth && s->depth;
    const float *dv = (const float *)(p + 4 * (size_t)npix);
    const uint8_t *sv = p + 8 * (size_t)npix;
    uint32_t csize = s->color ? (uint32_t)pixel_size(s->color_fmt) : 0, dsize = depth_size(s->depth_fmt);
    float scale = ldexpf(1, (int)bits);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint32_t idx = tile_index(x, y, w), i = y * w + x;
            if (color) {
                Rgba c;
                memcpy(&c, p + 4 * (size_t)i, 4);
                color_encode(s->color + idx * csize, s->color_fmt, c);
            }
            if (depth) {
                uint32_t z = (uint32_t)(dv[i] * scale);
                uint8_t *dp = s->depth + idx * dsize;
                memcpy(dp, &z, dsize == 2 ? 2 : 3);
                if (s->depth_fmt == 3)
                    dp[3] = sv[i];
            }
        }
}

/* The copies between a surface's images and an area at `at`, laid out as
   upload() and download() have it. */
static void regions(const Surface *s, VkDeviceSize at, VkBufferImageCopy *cc, VkBufferImageCopy dc[2])
{
    VkDeviceSize npix = (VkDeviceSize)s->w * s->h;
    VkExtent3D e = { s->w, s->h, 1 };
    *cc = (VkBufferImageCopy){ .bufferOffset = at, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                               .imageExtent = e };
    dc[0] = (VkBufferImageCopy){ .bufferOffset = at + 4 * npix,
                                 .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 }, .imageExtent = e };
    dc[1] = (VkBufferImageCopy){ .bufferOffset = at + 8 * npix,
                                 .imageSubresource = { VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1 }, .imageExtent = e };
}

/* Batches share their submission's buffers, each taking the next part.
   ring_room: whether n bytes at `align` still fit; with nothing recorded the
   buffer can grow instead. */
static int ring_room(Buffer *b, VkDeviceSize n, VkDeviceSize align)
{
    VkDeviceSize at = (b->used + align - 1) / align * align;
    if (at + n <= b->size)
        return 1;
    if (recording)
        return 0;
    ensure(b, n);
    return 1;
}

static VkDeviceSize ring_take(Buffer *b, VkDeviceSize n, VkDeviceSize align)
{
    VkDeviceSize at = (b->used + align - 1) / align * align;
    b->used = at + n;
    return at;
}

static VkDeviceSize tables_at = (VkDeviceSize)-1;     /* this submission's last copy of the tables */

static void begin_recording(void)
{
    if (recording)
        return;
    check(vkResetFences(device, 1, &done), "vkResetFences");
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(cmd, &bi);
    recording = 1;
}

static void end_pass(void)
{
    if (in_pass) {
        vkCmdEndRenderPass(cmd);
        in_pass = NULL;
    }
}

void vkr_sync(void)
{
    if (state != 1)
        return;
    if (recording) {
        end_pass();
        VkDeviceSize total = 0;
        for (int i = 0; i < NSURF; i++)
            if (surf[i].dirty_color || surf[i].dirty_depth) {
                surf[i].down_at = total;
                total += 9 * (VkDeviceSize)surf[i].w * surf[i].h;
            }
        if (total) {
            ensure(&down, total);       /* (no recorded command uses it yet) */
            barrier(ATTACH_STAGES, ATTACH_RW, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            for (int i = 0; i < NSURF; i++) {
                Surface *s = &surf[i];
                if (!s->dirty_color && !s->dirty_depth)
                    continue;
                VkBufferImageCopy cc, dc[2];
                regions(s, s->down_at, &cc, dc);
                vkCmdCopyImageToBuffer(cmd, s->cimg, VK_IMAGE_LAYOUT_GENERAL, down.buf, 1, &cc);
                vkCmdCopyImageToBuffer(cmd, s->ds, VK_IMAGE_LAYOUT_GENERAL, down.buf, 2, dc);
            }
            barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                    VK_ACCESS_HOST_READ_BIT);
        }
        vkEndCommandBuffer(cmd);
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
        uint64_t t0 = gpu_now_ns();
        check(vkQueueSubmit(queue, 1, &si, done), "vkQueueSubmit");
        check(vkWaitForFences(device, 1, &done, VK_TRUE, UINT64_MAX), "vkWaitForFences");
        uint64_t t1 = gpu_now_ns();
        vt[VKT_WAIT] += t1 - t0;
        submits++;
        vt_submits++;
        for (int i = 0; i < NSURF; i++) {
            Surface *s = &surf[i];
            if (!s->dirty_color && !s->dirty_depth)
                continue;
            download(s, down.ptr + s->down_at);
            s->dirty_color = s->dirty_depth = 0;
            s->hash = surface_hash(s);
        }
        vt[VKT_DOWNLOAD] += gpu_now_ns() - t1;
        recording = 0;
        epoch++;
        ubo.used = vbo.used = up.used = tables.used = 0;
        tables_at = (VkDeviceSize)-1;
        vkResetDescriptorPool(device, dpool, 0);
        nsets = 0;
    }
    /* From here others may write 3DS memory: check it before the next use. */
    for (int i = 0; i < NSURF; i++)
        surf[i].verified = 0;
    mem_epoch++;
}

static void vkr_draw_(const VkrTarget *t, const VkrVertex *v, uint32_t n);
void vkr_draw(const VkrTarget *t, const VkrVertex *v, uint32_t n)
{
    uint64_t t0 = gpu_now_ns();
    vkr_draw_(t, v, n);
    vt[VKT_RECORD] += gpu_now_ns() - t0;
}

void vkr_report(void)
{
    if (state != 1)
        return;
    /* (vkr_draw's total includes the hashing, uploads, textures and syncs it caused) */
    fprintf(stderr, "vulkan: %llu submissions, %llu uploads; ms: in vkr_draw %.0f (hash %.0f, upload %.0f, "
            "textures %.0f), waiting for the GPU %.0f, read-back %.0f\n", (unsigned long long)vt_submits,
            (unsigned long long)vt_uploads, vt[VKT_RECORD] / 1e6, vt[VKT_HASH] / 1e6, vt[VKT_UPLOAD] / 1e6,
            vt[VKT_TEXTURE] / 1e6, vt[VKT_WAIT] / 1e6, vt[VKT_DOWNLOAD] / 1e6);
    memset(vt, 0, sizeof vt);
    vt_submits = vt_uploads = 0;
}

static void vkr_draw_(const VkrTarget *t, const VkrVertex *v, uint32_t n)
{
    uint32_t w = t->width, h = t->height, npix = w * h;
    if (!n || !npix)
        return;

    /* the scissor box, in rows from the first memory row */
    VkRect2D scissor = { { 0, 0 }, { w, h } };
    if ((pica.reg[REG_SCISSOR_MODE] & 3) == 3) {
        uint32_t pos = pica.reg[REG_SCISSOR_POS], dim = pica.reg[REG_SCISSOR_DIM];
        int x0 = (int)(pos & 0x3FF), y0 = (int)(pos >> 16 & 0x3FF), x1 = (int)(dim & 0x3FF), y1 = (int)(dim >> 16 & 0x3FF);
        int r0 = (int)h - 1 - y1, r1 = (int)h - 1 - y0;     /* window y counts from the last row */
        if (x0 < 0) x0 = 0;
        if (r0 < 0) r0 = 0;
        if (x1 > (int)w - 1) x1 = (int)w - 1;
        if (r1 > (int)h - 1) r1 = (int)h - 1;
        if (x1 < x0 || r1 < r0)
            return;
        scissor = (VkRect2D){ { x0, r0 }, { (uint32_t)(x1 - x0 + 1), (uint32_t)(r1 - r0 + 1) } };
    }

    /* the fixed-function state */
    PipeKey k;
    memset(&k, 0, sizeof k);
    uint32_t dcm = pica.reg[REG_DEPTH_COLOR_MASK], st = pica.reg[REG_STENCIL_TEST], so = pica.reg[REG_STENCIL_OP];
    if (pica.reg[REG_COLOR_OPERATION] >> 8 & 1) {
        uint32_t bf = pica.reg[REG_BLEND_FUNC];
        k.blend = 1;
        k.rgb_eq = bf & 7;
        k.alpha_eq = bf >> 8 & 7;
        k.rgb_src = bf >> 16 & 0xF;
        k.rgb_dst = bf >> 20 & 0xF;
        k.alpha_src = bf >> 24 & 0xF;
        k.alpha_dst = bf >> 28;
    } else {
        k.logic_op = pica.reg[REG_LOGIC_OP] & 0xF;
        k.logic = k.logic_op != 3;      /* copy: no logic op needed */
    }
    k.mask = t->color && t->color_write ? dcm >> 8 & 0xF : 0;
    if (t->depth) {
        /* Vulkan writes depth only with its test on: an "always" test then. */
        int test = (dcm & 1) && t->depth_read;
        k.depth_write = t->depth_write && (dcm >> 12 & 1);
        k.depth_test = test || k.depth_write;
        k.depth_op = test ? dcm >> 4 & 7 : 1;
        if ((st & 1) && t->depth_fmt == 3) {
            k.stencil = 1;
            k.stencil_func = st >> 4 & 7;
            k.stencil_fail = so & 7;
            k.stencil_zfail = so >> 4 & 7;
            k.stencil_pass = so >> 8 & 7;
        }
    }
    int stencil_write = k.stencil && t->stencil_write;

    /* The target's surface. Another one on the same memory must have put
       what it drew there first (this one checks memory below). */
    Surface *s = get_surface(t);
    for (int i = 0; i < NSURF; i++)
        if (&surf[i] != s && (surf[i].dirty_color || surf[i].dirty_depth) &&
            (surface_covers(&surf[i], s->color, color_bytes(s)) || surface_covers(&surf[i], s->depth, depth_bytes(s)))) {
            vkr_sync();
            break;
        }

    /* Room in this submission (its buffers and descriptor sets) for the batch. */
    VkDeviceSize vsize = n * sizeof *v, upsize = 9 * (VkDeviceSize)npix;
    if (!ring_room(&vbo, vsize, 16) || !ring_room(&ubo, 256, ubo_align) || !ring_room(&up, upsize, 16) ||
        !ring_room(&tables, TAB_WORDS * 4, sbo_align) || nsets == NSETS) {
        vkr_sync();
        ring_room(&vbo, vsize, 16);
        ring_room(&up, upsize, 16);
    }

    /* the textures (decoding one may submit what is recorded) */
    int used = units_read();
    Texture *tex[3] = { NULL, NULL, NULL };
    for (int i = 0; i < 3; i++)
        if ((used >> i & 1) && !unit_unset(i))
            tex[i] = get_texture(i);
    for (int i = 0; i < 3; i++)
        if (tex[i])
            tex[i]->epoch = epoch;

    begin_recording();
    uint32_t bits = t->depth_fmt == 0 ? 16 : 24;
    if (!s->verified) {
        uint64_t hash = surface_hash(s);
        if (!s->valid || hash != s->hash) {
            end_pass();
            VkDeviceSize at = ring_take(&up, upsize, 16);
            uint64_t t0 = gpu_now_ns();
            upload(s, up.ptr + at);
            vt[VKT_UPLOAD] += gpu_now_ns() - t0;
            vt_uploads++;
            /* Everything is uploaded, so the old contents can go: UNDEFINED -> GENERAL. */
            VkImageMemoryBarrier ib[2] = {
                { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                  .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                  .image = s->cimg, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } },
                { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                  .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                  .image = s->ds,
                  .subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1 } },
            };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL,
                                 0, NULL, 2, ib);
            VkBufferImageCopy cc, dc[2];
            regions(s, at, &cc, dc);
            vkCmdCopyBufferToImage(cmd, up.buf, s->cimg, VK_IMAGE_LAYOUT_GENERAL, 1, &cc);
            vkCmdCopyBufferToImage(cmd, up.buf, s->ds, VK_IMAGE_LAYOUT_GENERAL, 2, dc);
            barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, ATTACH_STAGES, ATTACH_RW);
            s->hash = hash;
            s->valid = 1;
            uploads++;
        }
        s->verified = 1;
    }
    if (in_pass != s) {
        end_pass();
        barrier(ATTACH_STAGES | VK_PIPELINE_STAGE_TRANSFER_BIT, ATTACH_RW | VK_ACCESS_TRANSFER_WRITE_BIT,
                ATTACH_STAGES, ATTACH_RW);
        VkRenderPassBeginInfo rp = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = pass,
                                     .framebuffer = s->fb, .renderArea = { { 0, 0 }, { w, h } } };
        vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
        in_pass = s;
    }

    /* this batch's vertices, registers and tables */
    VkDeviceSize voff = ring_take(&vbo, vsize, 16);
    memcpy(vbo.ptr + voff, v, vsize);
    VkDeviceSize uoff = ring_take(&ubo, 256, ubo_align);
    uint32_t *u = (uint32_t *)(ubo.ptr + uoff);
    memset(u, 0, 256);
    for (int i = 0; i < 6; i++)
        for (int j = 0; j < 5; j++)
            u[5 * i + j] = pica.reg[tev_base[i] + j];
    u[30] = pica.reg[REG_TEXENV_BUFFER_COLOR];
    u[31] = pica.reg[REG_TEXENV_UPDATE_BUFFER];
    u[32] = pica.reg[REG_ALPHA_TEST];
    u[33] = bits;
    u[34] = (uint32_t)used;
    u[36] = unset_color();
    u[37] = (uint32_t)lighting_read();
    if (u[37] || (pica.reg[REG_TEXENV_UPDATE_BUFFER] & 7) == 5) {
        static uint32_t tw[TAB_WORDS];
        memcpy(tw, &pica.reg[0x140], (0x1DA - 0x140) * 4);
        tw[TAB_FOG_COLOR] = pica.reg[REG_FOG_COLOR];
        memcpy(tw + TAB_LIGHTING, pica.lighting_lut, sizeof pica.lighting_lut);
        memcpy(tw + TAB_FOG, pica.fog_lut, sizeof pica.fog_lut);
        if (tables_at == (VkDeviceSize)-1 || memcmp(tables.ptr + tables_at, tw, sizeof tw) != 0) {
            tables_at = ring_take(&tables, sizeof tw, sbo_align);
            memcpy(tables.ptr + tables_at, tw, sizeof tw);
        }
    }
    VkDescriptorImageInfo views[3];
    for (int i = 0; i < 3; i++) {
        views[i] = (VkDescriptorImageInfo){ sampler, dummy_view, VK_IMAGE_LAYOUT_GENERAL };
        if (!(used >> i & 1))
            continue;
        if (!tex[i]) {
            u[35] |= 1u << i;
            continue;
        }
        TexUnit tu;
        tex_unit_read(i, &tu);
        uint32_t *w8 = &u[40 + 8 * i];
        w8[0] = tu.width;
        w8[1] = tu.height;
        w8[2] = tu.param;
        w8[3] = tu.lod;
        w8[4] = tu.border;
        w8[5] = (uint32_t)tu.type;
        w8[7] = i == 2 && (pica.reg[REG_TEXUNIT_CONFIG] >> 13 & 1) ? 1 : (uint32_t)i;
        views[i].imageView = tex[i]->view;
    }
    VkDescriptorSet set;
    VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool,
                                       .descriptorSetCount = 1, .pSetLayouts = &set_layout };
    check(vkAllocateDescriptorSets(device, &ai, &set), "vkAllocateDescriptorSets");
    nsets++;
    VkDescriptorBufferInfo bi = { ubo.buf, uoff, 256 },
                           ti = { tables.buf, tables_at == (VkDeviceSize)-1 ? 0 : tables_at, TAB_WORDS * 4 };
    VkWriteDescriptorSet dw[3] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = 3,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = views },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 2, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &ti },
    };
    vkUpdateDescriptorSets(device, 3, dw, 0, NULL);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, get_pipeline(&k));
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
    vkCmdBindVertexBuffers(cmd, 0, 1, &vbo.buf, &voff);
    VkViewport vp = { 0, 0, (float)w, (float)h, 0, 1 };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    uint32_t bc = pica.reg[REG_BLEND_COLOR];
    float consts[4] = { (float)(bc & 0xFF) / 255, (float)(bc >> 8 & 0xFF) / 255, (float)(bc >> 16 & 0xFF) / 255,
                        (float)(bc >> 24) / 255 };
    vkCmdSetBlendConstants(cmd, consts);
    vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, st >> 24);
    vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, stencil_write ? st >> 8 & 0xFF : 0);
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, st >> 16 & 0xFF);
    vkCmdDraw(cmd, n, 1, 0, 0);

    if (k.mask)
        s->dirty_color = 1;
    if (k.depth_write || stencil_write)
        s->dirty_depth = 1;
    batches++;
}
