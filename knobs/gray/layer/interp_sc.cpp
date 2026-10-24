// interp_sc.cpp — 插帧的子图层送显后端 (说明见 interp_sc.h)。
#include "interp_sc.h"

#include <android/log.h>
#include <dlfcn.h>
#include <sys/system_properties.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

#if __has_include(<vulkan/vulkan_android.h>) && __has_include(<android/hardware_buffer.h>)
#include <android/hardware_buffer.h>
#include <vulkan/vulkan_android.h>
#define SC_HAVE_ANDROID 1
#endif

#define SC_LOG(...) __android_log_print(ANDROID_LOG_INFO, "refknobs", "interp_sc: " __VA_ARGS__)

namespace {

std::mutex g_m;
int g_req = -1;   // -1 未读属性
ANativeWindow* g_window = nullptr;
std::string g_reason;
uint64_t g_shown = 0, g_txn = 0, g_alloc = 0;
int32_t g_dst_w = 0, g_dst_h = 0;

bool requested() {
    if (g_req < 0) {
        char v[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.knobs.interp_backend", v);
        g_req = !strcmp(v, "sc") ? 1 : 0;
        char d[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.knobs.interp_sc_dst", d);   // 子图层在父图层坐标里的大小, 缺省取窗口尺寸
        unsigned w = 0, h = 0;
        if (sscanf(d, "%ux%u", &w, &h) == 2) { g_dst_w = (int32_t)w; g_dst_h = (int32_t)h; }
    }
    return g_req == 1;
}

#ifdef SC_HAVE_ANDROID
// libandroid 的 ASurfaceControl / ASurfaceTransaction (API 29+): dlsym, 不加链接依赖
struct ASurfaceControl;
struct ASurfaceTransaction;
struct ARectI { int32_t left, top, right, bottom; };
struct Api {
    ASurfaceControl* (*createFromWindow)(ANativeWindow*, const char*) = nullptr;
    void (*release)(ASurfaceControl*) = nullptr;
    ASurfaceTransaction* (*txnCreate)() = nullptr;
    void (*txnDelete)(ASurfaceTransaction*) = nullptr;
    void (*txnApply)(ASurfaceTransaction*) = nullptr;
    void (*setBuffer)(ASurfaceTransaction*, ASurfaceControl*, AHardwareBuffer*, int) = nullptr;
    void (*setVisibility)(ASurfaceTransaction*, ASurfaceControl*, int8_t) = nullptr;
    void (*setGeometry)(ASurfaceTransaction*, ASurfaceControl*, const ARectI*, const ARectI*, int32_t) = nullptr;
    void (*setDesiredPresentTime)(ASurfaceTransaction*, int64_t) = nullptr;
    void (*setZOrder)(ASurfaceTransaction*, ASurfaceControl*, int32_t) = nullptr;
    void (*reparent)(ASurfaceTransaction*, ASurfaceControl*, ASurfaceControl*) = nullptr;
    int32_t (*winW)(ANativeWindow*) = nullptr;
    int32_t (*winH)(ANativeWindow*) = nullptr;
    int (*ahbAlloc)(const AHardwareBuffer_Desc*, AHardwareBuffer**) = nullptr;
    void (*ahbRelease)(AHardwareBuffer*) = nullptr;
    bool ok = false;
} g_api;

bool load_api() {
    if (g_api.ok) return true;
    void* h = dlopen("libandroid.so", RTLD_NOW);
    void* nw = dlopen("libnativewindow.so", RTLD_NOW);
    if (!h || !nw) { g_reason = "dlopen"; return false; }
#define SC_SYM(lib, field, name) g_api.field = reinterpret_cast<decltype(g_api.field)>(dlsym(lib, name));
    SC_SYM(h, createFromWindow, "ASurfaceControl_createFromWindow")
    SC_SYM(h, release, "ASurfaceControl_release")
    SC_SYM(h, txnCreate, "ASurfaceTransaction_create")
    SC_SYM(h, txnDelete, "ASurfaceTransaction_delete")
    SC_SYM(h, txnApply, "ASurfaceTransaction_apply")
    SC_SYM(h, setBuffer, "ASurfaceTransaction_setBuffer")
    SC_SYM(h, setVisibility, "ASurfaceTransaction_setVisibility")
    SC_SYM(h, setGeometry, "ASurfaceTransaction_setGeometry")
    SC_SYM(h, setDesiredPresentTime, "ASurfaceTransaction_setDesiredPresentTime")
    SC_SYM(h, setZOrder, "ASurfaceTransaction_setZOrder")
    SC_SYM(h, reparent, "ASurfaceTransaction_reparent")
    SC_SYM(nw, winW, "ANativeWindow_getWidth")
    SC_SYM(nw, winH, "ANativeWindow_getHeight")
    SC_SYM(nw, ahbAlloc, "AHardwareBuffer_allocate")
    SC_SYM(nw, ahbRelease, "AHardwareBuffer_release")
#undef SC_SYM
    g_api.ok = g_api.createFromWindow && g_api.txnCreate && g_api.txnApply && g_api.setBuffer && g_api.setVisibility &&
               g_api.setGeometry && g_api.setDesiredPresentTime && g_api.ahbAlloc;
    if (!g_api.ok) g_reason = "missing_ndk_symbols";
    return g_api.ok;
}

struct Buf { VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; AHardwareBuffer* ahb = nullptr; ScBuf pub; };
struct Dev {
    VkDevice dev = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    bool ext = false;
    PFN_vkCreateImage CreateImage = nullptr;
    PFN_vkDestroyImage DestroyImage = nullptr;
    PFN_vkAllocateMemory AllocateMemory = nullptr;
    PFN_vkFreeMemory FreeMemory = nullptr;
    PFN_vkBindImageMemory BindImageMemory = nullptr;
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID GetAhbProps = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties gpmp = nullptr;
    std::vector<Buf> ring;
    uint32_t w = 0, h = 0, next = 0;
    ASurfaceControl* sc = nullptr;
    bool geometry_set = false;
};
std::map<VkDevice, Dev> g_devs;

Dev* dev_of(VkDevice d) {
    auto it = g_devs.find(d);
    if (it == g_devs.end() && g_devs.size() == 1) it = g_devs.begin();
    return it == g_devs.end() ? nullptr : &it->second;
}

void free_ring(Dev& D) {
    for (auto& b : D.ring) {
        if (b.img) D.DestroyImage(D.dev, b.img, nullptr);
        if (b.mem) D.FreeMemory(D.dev, b.mem, nullptr);
        if (b.ahb && g_api.ahbRelease) g_api.ahbRelease(b.ahb);
    }
    D.ring.clear();
}

bool make_buf(Dev& D, Buf& b, uint32_t w, uint32_t h) {
    AHardwareBuffer_Desc desc{};
    desc.width = w; desc.height = h; desc.layers = 1; desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    desc.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                 AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;
    if (g_api.ahbAlloc(&desc, &b.ahb) != 0 || !b.ahb) { g_reason = "ahb_allocate"; return false; }
    VkAndroidHardwareBufferFormatPropertiesANDROID fp{VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
    VkAndroidHardwareBufferPropertiesANDROID pp{VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID, &fp};
    if (D.GetAhbProps(D.dev, b.ahb, &pp) != VK_SUCCESS) { g_reason = "ahb_properties"; return false; }
    VkExternalMemoryImageCreateInfo emi{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, nullptr,
                                        VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &emi};
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_R8G8B8A8_UNORM; ici.extent = {w, h, 1};
    ici.mipLevels = 1; ici.arrayLayers = 1; ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (D.CreateImage(D.dev, &ici, nullptr, &b.img) != VK_SUCCESS) { g_reason = "ahb_image"; return false; }
    VkPhysicalDeviceMemoryProperties mp{};
    D.gpmp(D.phys, &mp);
    uint32_t mt = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if (pp.memoryTypeBits & (1u << i)) { mt = i; break; }
    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, nullptr, b.img, VK_NULL_HANDLE};
    VkImportAndroidHardwareBufferInfoANDROID imp{VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, &ded, b.ahb};
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, pp.allocationSize, mt};
    if (mt == UINT32_MAX || D.AllocateMemory(D.dev, &mai, nullptr, &b.mem) != VK_SUCCESS) { g_reason = "ahb_import"; return false; }
    if (D.BindImageMemory(D.dev, b.img, b.mem, 0) != VK_SUCCESS) { g_reason = "ahb_bind"; return false; }
    b.pub.img = b.img; b.pub.ahb = b.ahb; b.pub.fresh = true;
    g_alloc++;
    return true;
}
#endif

}  // namespace

std::vector<const char*> sc_wanted_device_exts(const std::vector<std::string>& available, const std::vector<std::string>& already) {
    std::vector<const char*> out;
#ifdef SC_HAVE_ANDROID
    if (!requested()) return out;
    static const char* want[] = {VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
                                 VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
                                 VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
                                 VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, VK_KHR_MAINTENANCE1_EXTENSION_NAME,
                                 VK_KHR_BIND_MEMORY_2_EXTENSION_NAME};
    for (const char* w : want) {
        bool have = false, on = false;
        for (auto& a : available) if (a == w) have = true;
        for (auto& a : already) if (a == w) on = true;
        if (have && !on) out.push_back(w);
    }
#else
    (void)available; (void)already;
#endif
    return out;
}

bool sc_requested() { return requested(); }

void sc_on_window(ANativeWindow* w) {
    std::lock_guard<std::mutex> lk(g_m);
    g_window = w;
}

void sc_on_device(VkDevice dev, VkPhysicalDevice phys, PFN_vkGetDeviceProcAddr gdpa, PFN_vkGetPhysicalDeviceMemoryProperties gpmp,
                  bool ext_enabled) {
#ifdef SC_HAVE_ANDROID
    if (!requested()) return;
    std::lock_guard<std::mutex> lk(g_m);
    Dev D;
    D.dev = dev; D.phys = phys; D.ext = ext_enabled; D.gpmp = gpmp;
    D.CreateImage = (PFN_vkCreateImage)gdpa(dev, "vkCreateImage");
    D.DestroyImage = (PFN_vkDestroyImage)gdpa(dev, "vkDestroyImage");
    D.AllocateMemory = (PFN_vkAllocateMemory)gdpa(dev, "vkAllocateMemory");
    D.FreeMemory = (PFN_vkFreeMemory)gdpa(dev, "vkFreeMemory");
    D.BindImageMemory = (PFN_vkBindImageMemory)gdpa(dev, "vkBindImageMemory");
    D.GetAhbProps = (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)gdpa(dev, "vkGetAndroidHardwareBufferPropertiesANDROID");
    if (!ext_enabled) g_reason = "ahb_extension_not_enabled";
    else if (!D.GetAhbProps || !gpmp) g_reason = "ahb_functions_missing";
    else if (!load_api()) {}
    g_devs[dev] = D;
    SC_LOG("device: ext=%d props_fn=%d api=%d reason=%s", (int)ext_enabled, D.GetAhbProps ? 1 : 0, (int)g_api.ok, g_reason.c_str());
#else
    (void)dev; (void)phys; (void)gdpa; (void)gpmp; (void)ext_enabled;
#endif
}

bool sc_ready(VkDevice dev) {
#ifdef SC_HAVE_ANDROID
    std::lock_guard<std::mutex> lk(g_m);
    Dev* D = dev_of(dev);
    if (!D || !D->ext || !D->GetAhbProps || !g_api.ok || !g_window) {
        if (!g_window && g_reason.empty()) g_reason = "no_window";
        return false;
    }
    if (!D->sc) {
        D->sc = g_api.createFromWindow(g_window, "selfsr_interp");
        if (!D->sc) { g_reason = "create_surface_control"; return false; }
        SC_LOG("子图层已建");
    }
    return true;
#else
    (void)dev;
    return false;
#endif
}

ScBuf* sc_next_buffer(VkDevice dev, uint32_t w, uint32_t h, uint32_t ring) {
#ifdef SC_HAVE_ANDROID
    std::lock_guard<std::mutex> lk(g_m);
    Dev* D = dev_of(dev);
    if (!D) return nullptr;
    if (D->w != w || D->h != h || D->ring.size() != ring) {
        free_ring(*D);
        D->ring.resize(ring);
        for (auto& b : D->ring) if (!make_buf(*D, b, w, h)) { free_ring(*D); return nullptr; }
        D->w = w; D->h = h; D->next = 0; D->geometry_set = false;
        SC_LOG("缓冲环 %u 张 %ux%u", ring, w, h);
    }
    Buf& b = D->ring[D->next++ % D->ring.size()];
    return &b.pub;
#else
    (void)dev; (void)w; (void)h; (void)ring;
    return nullptr;
#endif
}

void sc_release_barrier(VkCommandBuffer cb, PFN_vkCmdPipelineBarrier bar, VkImage img, uint32_t family) {
#ifdef SC_HAVE_ANDROID
    VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; ib.dstAccessMask = 0;
    ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.srcQueueFamilyIndex = family; ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    ib.image = img; ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    bar(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
#else
    (void)cb; (void)bar; (void)img; (void)family;
#endif
}

void sc_acquire_barrier(VkCommandBuffer cb, PFN_vkCmdPipelineBarrier bar, VkImage img, uint32_t family, bool fresh) {
#ifdef SC_HAVE_ANDROID
    VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    ib.srcAccessMask = 0; ib.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    ib.srcQueueFamilyIndex = fresh ? VK_QUEUE_FAMILY_IGNORED : VK_QUEUE_FAMILY_FOREIGN_EXT;
    ib.dstQueueFamilyIndex = fresh ? VK_QUEUE_FAMILY_IGNORED : family;
    ib.image = img; ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    bar(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
#else
    (void)cb; (void)bar; (void)img; (void)family; (void)fresh;
#endif
}

void sc_show(VkDevice dev, const std::vector<std::pair<ScBuf*, int64_t>>& frames, uint32_t w, uint32_t h) {
#ifdef SC_HAVE_ANDROID
    std::lock_guard<std::mutex> lk(g_m);
    Dev* D = dev_of(dev);
    if (!D || !D->sc) return;
    for (size_t i = 0; i < frames.size(); i++) {
        ASurfaceTransaction* t = g_api.txnCreate();
        if (!t) continue;
        if (!D->geometry_set) {
            int32_t dw = g_dst_w ? g_dst_w : (g_api.winW ? g_api.winW(g_window) : (int32_t)w);
            int32_t dh = g_dst_h ? g_dst_h : (g_api.winH ? g_api.winH(g_window) : (int32_t)h);
            ARectI src{0, 0, (int32_t)w, (int32_t)h}, dst{0, 0, dw, dh};
            g_api.setGeometry(t, D->sc, &src, &dst, 0);
            g_api.setVisibility(t, D->sc, 1);
            if (g_api.setZOrder) g_api.setZOrder(t, D->sc, 1);
            D->geometry_set = true;
            SC_LOG("子图层几何: 源 %ux%u → 目标 %dx%d", w, h, dw, dh);
        }
        g_api.setBuffer(t, D->sc, static_cast<AHardwareBuffer*>(frames[i].first->ahb), -1);
        if (frames[i].second > 0) g_api.setDesiredPresentTime(t, frames[i].second);
        g_api.txnApply(t);
        g_api.txnDelete(t);
        frames[i].first->fresh = false;
        g_txn++; g_shown++;
    }
#else
    (void)dev; (void)frames; (void)w; (void)h;
#endif
}

void sc_on_destroy_device(VkDevice dev) {
#ifdef SC_HAVE_ANDROID
    std::lock_guard<std::mutex> lk(g_m);
    auto it = g_devs.find(dev);
    if (it == g_devs.end()) return;
    Dev& D = it->second;
    if (D.sc) {
        if (g_api.txnCreate && g_api.reparent) {   // 从窗口上摘掉子图层
            ASurfaceTransaction* t = g_api.txnCreate();
            g_api.reparent(t, D.sc, nullptr);
            g_api.txnApply(t);
            g_api.txnDelete(t);
        }
        if (g_api.release) g_api.release(D.sc);
    }
    free_ring(D);
    g_devs.erase(it);
#else
    (void)dev;
#endif
}

std::string sc_json() {
    std::lock_guard<std::mutex> lk(g_m);
    char b[256];
    snprintf(b, sizeof b, "{\"requested\":%s,\"window\":%s,\"shown\":%llu,\"buffers_allocated\":%llu,\"reason\":\"%s\"}",
             g_req == 1 ? "true" : "false", g_window ? "true" : "false", (unsigned long long)g_shown,
             (unsigned long long)g_alloc, g_reason.c_str());
    return b;
}
