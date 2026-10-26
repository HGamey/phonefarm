// interp_sc.cpp — 插帧的子图层送显后端 (说明见 interp_sc.h)。
#include "interp_sc.h"

#include <android/log.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/system_properties.h>

#include <cstdio>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <time.h>

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
int g_pacer = 0;               // debug.knobs.interp_sc_pacer=1: 层自己的送显线程按时 apply, 不带 desiredPresentTime
long g_margin_ns = 3000000;    // debug.knobs.interp_sc_margin_ns: 比定的上屏时刻提前多久 apply
uint64_t g_pacer_late = 0;     // 轮到时已经过了定的时刻
bool g_vote_fixed = false;     // debug.knobs.interp_sc_vote=fixed
int32_t g_xform_used = 0;
int32_t g_xform = -1;          // debug.knobs.interp_sc_xform: 强制子图层 setGeometry 的 transform (ANATIVEWINDOW_TRANSFORM_*); 缺省按 preTransform 自动

bool requested() {
    if (g_req < 0) {
        char v[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.knobs.interp_backend", v);
        g_req = !strcmp(v, "sc") ? 1 : 0;
        char d[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.knobs.interp_sc_dst", d);   // 子图层在父图层坐标里的大小, 缺省 = 缓冲大小
        unsigned w = 0, h = 0;
        if (sscanf(d, "%ux%u", &w, &h) == 2) { g_dst_w = (int32_t)w; g_dst_h = (int32_t)h; }
        char pc[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.knobs.interp_sc_pacer", pc);
        g_pacer = pc[0] == '1' ? 1 : pc[0] == '2' ? 2 : 0;
        char mg[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.knobs.interp_sc_margin_ns", mg);
        if (mg[0]) g_margin_ns = atol(mg);
        char vt[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.knobs.interp_sc_vote", vt);
        g_vote_fixed = !strcmp(vt, "fixed");
        char xf[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.knobs.interp_sc_xform", xf);
        if (xf[0]) g_xform = atoi(xf);
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
    void (*setFrameRate)(ASurfaceTransaction*, ASurfaceControl*, float, int8_t) = nullptr;   // API 30
    void (*setFrameRateCS)(ASurfaceTransaction*, ASurfaceControl*, float, int8_t, int8_t) = nullptr;   // API 31
    void (*setTransparency)(ASurfaceTransaction*, ASurfaceControl*, int8_t) = nullptr;
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
    SC_SYM(h, setTransparency, "ASurfaceTransaction_setBufferTransparency")
    SC_SYM(h, setFrameRateCS, "ASurfaceTransaction_setFrameRateWithChangeStrategy")
    SC_SYM(h, setVisibility, "ASurfaceTransaction_setVisibility")
    SC_SYM(h, setGeometry, "ASurfaceTransaction_setGeometry")
    SC_SYM(h, setDesiredPresentTime, "ASurfaceTransaction_setDesiredPresentTime")
    SC_SYM(h, setZOrder, "ASurfaceTransaction_setZOrder")
    SC_SYM(h, reparent, "ASurfaceTransaction_reparent")
    SC_SYM(h, setFrameRate, "ASurfaceTransaction_setFrameRate")
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
    PFN_vkCreateFence CreateFence = nullptr;
    PFN_vkDestroyFence DestroyFence = nullptr;
    PFN_vkQueueSubmit QueueSubmit = nullptr;
    PFN_vkGetFenceFdKHR GetFenceFd = nullptr;
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
                                 VK_KHR_BIND_MEMORY_2_EXTENSION_NAME, VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME,
                                 VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME};
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
    D.CreateFence = (PFN_vkCreateFence)gdpa(dev, "vkCreateFence");
    D.DestroyFence = (PFN_vkDestroyFence)gdpa(dev, "vkDestroyFence");
    D.QueueSubmit = (PFN_vkQueueSubmit)gdpa(dev, "vkQueueSubmit");
    D.GetFenceFd = (PFN_vkGetFenceFdKHR)gdpa(dev, "vkGetFenceFdKHR");   // 没开 external_fence_fd 时为空, 退回 CPU 等
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

#ifdef SC_HAVE_ANDROID
// 送显线程: SF 对带 desiredPresentTime 的事务是「某一帧合成时检查哪些到点了, 同一帧到点的只取最后一张」,
// 真机上合成帧常晚 1~2 个 vsync 或被顶掉 (帧早已画完, fence 在定的时刻前就 signal)。
// 改为层自己在「定的时刻 − margin」才 apply, 不带 desiredPresentTime, 一张一张交。
struct PacerJob { ASurfaceControl* sc; AHardwareBuffer* ahb; int fd; uint64_t when; };
std::mutex g_pm;
std::condition_variable g_pcv;
std::deque<PacerJob> g_pq;
bool g_pbusy = false, g_pstarted = false;

uint64_t mono_ns() { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec; }

void pacer_loop() {
    for (;;) {
        PacerJob j;
        {
            std::unique_lock<std::mutex> lk(g_pm);
            g_pcv.wait(lk, [] { return !g_pq.empty(); });
            j = g_pq.front(); g_pq.pop_front();
            g_pbusy = true;
        }
        uint64_t at = j.when > (uint64_t)g_margin_ns ? j.when - (uint64_t)g_margin_ns : 0;
        uint64_t now = mono_ns();
        if (at > now) {
            timespec ts{(time_t)(at / 1000000000ull), (long)(at % 1000000000ull)};
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
        } else if (now > j.when) {
            g_pacer_late++;
        }
        ASurfaceTransaction* t = g_api.txnCreate();
        if (t) {
            g_api.setBuffer(t, j.sc, j.ahb, j.fd);   // fd 归事务
            g_api.txnApply(t);
            g_api.txnDelete(t);
        } else if (j.fd >= 0) {
            close(j.fd);
        }
        { std::lock_guard<std::mutex> lk(g_pm); g_pbusy = false; }
        g_pcv.notify_all();
    }
}

// sc_pacer=2: 跟着 vsync 走。送显线程挂 AChoreographer 帧回调, 每个回调最多交一张「到点」的帧
// (到点 = 定的上屏时刻 ≤ 本次 vsync + g_margin_ns), 从机制上保证一个 vsync 只交一张。
struct ALooper; struct AChoreographer;
typedef void (*ChoreoCb64)(int64_t frameTimeNanos, void* data);
struct ChoreoApi {
    ALooper* (*prepare)(int) = nullptr;
    int (*pollOnce)(int, int*, int*, void**) = nullptr;
    AChoreographer* (*instance)() = nullptr;
    void (*post64)(AChoreographer*, ChoreoCb64, void*) = nullptr;
} g_ch;
AChoreographer* g_choreo = nullptr;
uint64_t g_vsync_cb = 0, g_vsync_sent = 0;

void choreo_cb(int64_t frame_ns, void*) {
    g_vsync_cb++;
    PacerJob j{}; bool have = false;
    {
        std::lock_guard<std::mutex> lk(g_pm);
        if (!g_pq.empty() && g_pq.front().when <= (uint64_t)frame_ns + (uint64_t)g_margin_ns) {
            j = g_pq.front(); g_pq.pop_front(); have = true; g_pbusy = true;
        }
    }
    if (have) {
        ASurfaceTransaction* t = g_api.txnCreate();
        if (t) { g_api.setBuffer(t, j.sc, j.ahb, j.fd); g_api.txnApply(t); g_api.txnDelete(t); }
        else if (j.fd >= 0) close(j.fd);
        g_vsync_sent++;
        { std::lock_guard<std::mutex> lk(g_pm); g_pbusy = false; }
        g_pcv.notify_all();
    }
    g_ch.post64(g_choreo, choreo_cb, nullptr);
}

void choreo_loop() {
    g_ch.prepare(0);
    g_choreo = g_ch.instance();
    if (!g_choreo) { SC_LOG("AChoreographer 拿不到, 退回睡眠送显"); g_pacer = 1; pacer_loop(); return; }
    g_ch.post64(g_choreo, choreo_cb, nullptr);
    for (;;) g_ch.pollOnce(-1, nullptr, nullptr, nullptr);
}

void pacer_push(const PacerJob& j) {
    std::lock_guard<std::mutex> lk(g_pm);
    if (!g_pstarted) {
        if (g_pacer == 2) {
            void* h = dlopen("libandroid.so", RTLD_NOW);
            g_ch.prepare = reinterpret_cast<decltype(g_ch.prepare)>(dlsym(h, "ALooper_prepare"));
            g_ch.pollOnce = reinterpret_cast<decltype(g_ch.pollOnce)>(dlsym(h, "ALooper_pollOnce"));
            g_ch.instance = reinterpret_cast<decltype(g_ch.instance)>(dlsym(h, "AChoreographer_getInstance"));
            g_ch.post64 = reinterpret_cast<decltype(g_ch.post64)>(dlsym(h, "AChoreographer_postFrameCallback64"));
            if (g_ch.prepare && g_ch.pollOnce && g_ch.instance && g_ch.post64) std::thread(choreo_loop).detach();
            else { g_pacer = 1; std::thread(pacer_loop).detach(); }
        } else {
            std::thread(pacer_loop).detach();
        }
        g_pstarted = true;
    }
    g_pq.push_back(j);
    g_pcv.notify_all();
}

// 设备销毁前: 丢掉没送的, 等正在送的那张送完
void pacer_flush() {
    std::unique_lock<std::mutex> lk(g_pm);
    for (auto& j : g_pq) if (j.fd >= 0) close(j.fd);
    g_pq.clear();
    g_pcv.wait_for(lk, std::chrono::milliseconds(200), [] { return !g_pbusy; });
}
#endif

void sc_show(VkDevice dev, const std::vector<std::pair<ScBuf*, int64_t>>& frames, uint32_t w, uint32_t h, uint32_t pre_transform, int fence_fd) {
#ifdef SC_HAVE_ANDROID
    std::lock_guard<std::mutex> lk(g_m);
    Dev* D = dev_of(dev);
    if (!D || !D->sc) return;
    for (size_t i = 0; i < frames.size(); i++) {
        ASurfaceTransaction* t = g_api.txnCreate();
        if (!t) continue;
        if (!D->geometry_set) {
            // 子图层在父图层 (游戏 SurfaceView) 的坐标里: 父图层自带「缓冲 → 屏幕」的缩放, 子图层跟着它缩放,
            // 所以目标大小 = 缓冲大小 (真机: 按窗口 2688x1216 设会被再放大 1.26 倍)
            // 游戏按 preTransform 预旋转时 (例 refbench: ROTATE_90), 父图层的缓冲带着反向的缓冲变换 (驱动设的),
            // 子图层不继承它: 这里设同样的反向变换, 目标宽高对调 (荣耀 AAP-AN00 实测, 不设时画面错位)
            int32_t xf = pre_transform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ? 7 /* ROTATE_270 */
                       : pre_transform == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR ? 3 /* ROTATE_180 */
                       : pre_transform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR ? 4 /* ROTATE_90 */ : 0;
            if (g_xform >= 0) xf = g_xform;
            bool swap = xf == 4 || xf == 7;
            int32_t dw = g_dst_w ? g_dst_w : (int32_t)(swap ? h : w);
            int32_t dh = g_dst_h ? g_dst_h : (int32_t)(swap ? w : h);
            ARectI src{0, 0, (int32_t)w, (int32_t)h}, dst{0, 0, dw, dh};
            g_api.setGeometry(t, D->sc, &src, &dst, xf);
            g_xform_used = xf;
            g_api.setVisibility(t, D->sc, 1);
            if (g_api.setZOrder) g_api.setZOrder(t, D->sc, 1);
            // 投票: 缺省 DEFAULT; debug.knobs.interp_sc_vote=fixed 时按「固定帧率内容」投 (FIXED_SOURCE, 切换策略 ALWAYS)
            if (g_vote_fixed && g_api.setFrameRateCS) g_api.setFrameRateCS(t, D->sc, 120.f, 1 /* FIXED_SOURCE */, 1 /* ALWAYS */);
            else if (g_api.setFrameRate) g_api.setFrameRate(t, D->sc, 120.f, 0 /* DEFAULT */);
            // 不透明: 真实帧是从游戏 swapchain 原样拷的, alpha 不保证是 1; 不声明的话 SF 会把下面的游戏图层混进来
            // (真机: 超分 + 插帧时 HUD 出现双影、整屏发灰)
            if (g_api.setTransparency) g_api.setTransparency(t, D->sc, 2 /* ASURFACE_TRANSACTION_TRANSPARENCY_OPAQUE */);
            D->geometry_set = true;
            SC_LOG("子图层几何: 源 %ux%u → 目标 %dx%d", w, h, dw, dh);
        }
        // 每个事务各拿一份 fence fd (setBuffer 接管所有权); 最后一个用原件
        int fd = fence_fd < 0 ? -1 : (i + 1 == frames.size() ? fence_fd : dup(fence_fd));
        if (g_pacer) {
            g_api.txnApply(t);   // 只带几何 (第一次) 的事务照常交; 缓冲交给送显线程按时交
            g_api.txnDelete(t);
            pacer_push({D->sc, static_cast<AHardwareBuffer*>(frames[i].first->ahb), fd, (uint64_t)frames[i].second});
        } else {
        g_api.setBuffer(t, D->sc, static_cast<AHardwareBuffer*>(frames[i].first->ahb), fd);
        if (frames[i].second > 0) g_api.setDesiredPresentTime(t, frames[i].second);
        g_api.txnApply(t);
        g_api.txnDelete(t);
        }
        frames[i].first->fresh = false;
        g_txn++; g_shown++;
    }
    if (frames.empty() && fence_fd >= 0) close(fence_fd);
#else
    (void)dev; (void)frames; (void)w; (void)h; (void)fence_fd;
#endif
}

int sc_export_fence(VkDevice dev, VkQueue q) {
#ifdef SC_HAVE_ANDROID
    Dev* D;
    { std::lock_guard<std::mutex> lk(g_m); D = dev_of(dev); }
    if (!D || !D->GetFenceFd || !D->CreateFence || !D->QueueSubmit) return -1;
    // 空提交挂一个可导出的 fence: 同一队列上前面的提交 (层的拷贝/合成) 做完它才 signal
    VkExportFenceCreateInfo efi{VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO, nullptr, VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, &efi, 0};
    VkFence f = VK_NULL_HANDLE;
    if (D->CreateFence(dev, &fci, nullptr, &f) != VK_SUCCESS) return -1;
    int fd = -1;
    if (D->QueueSubmit(q, 0, nullptr, f) == VK_SUCCESS) {
        VkFenceGetFdInfoKHR gi{VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR, nullptr, f, VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
        if (D->GetFenceFd(dev, &gi, &fd) != VK_SUCCESS) fd = -1;
    }
    D->DestroyFence(dev, f, nullptr);   // 只在 fd 拿到之后才销毁; 提交失败时 fence 从未入队, 可直接销毁
    return fd;
#else
    (void)dev; (void)q;
    return -1;
#endif
}

void sc_on_destroy_device(VkDevice dev) {
#ifdef SC_HAVE_ANDROID
    std::lock_guard<std::mutex> lk(g_m);
    auto it = g_devs.find(dev);
    if (it == g_devs.end()) return;
    Dev& D = it->second;
    pacer_flush();
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
    char b[400];
    snprintf(b, sizeof b, "{\"requested\":%s,\"window\":%s,\"shown\":%llu,\"buffers_allocated\":%llu,\"pacer\":%d,\"margin_ns\":%ld,\"pacer_late\":%llu,\"vsync_cb\":%llu,\"vsync_sent\":%llu,\"xform\":%d,\"reason\":\"%s\"}",
             g_req == 1 ? "true" : "false", g_window ? "true" : "false", (unsigned long long)g_shown,
             (unsigned long long)g_alloc, g_pacer, g_margin_ns, (unsigned long long)g_pacer_late, (unsigned long long)g_vsync_cb, (unsigned long long)g_vsync_sent, g_xform_used, g_reason.c_str());
    return b;
}
