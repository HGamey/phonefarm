// interp_sc.h — 插帧的「子图层送显」后端 (debug.knobs.interp_backend=sc)。
//
// 为什么要它: 红魔 (RedMagicOS 11) 改过的 libgui.so 里有进程内限帧器 (FpsControl / IFAS, setLimitedFps),
// 应用经缓冲队列 (vkQueuePresentKHR → ANativeWindow queueBuffer) 送显的总次数被卡在系统定的目标帧率 (过热 30/s);
// 层多送的合成帧也算在里面, 结果只是把一半真实帧换成合成帧 (第 4 步实测)。
// 做法: 在游戏窗口下挂一个 ASurfaceControl 子图层 (盖在游戏图层上面), 真实帧拷一份、合成帧画进层自己的
// AHardwareBuffer (导入成 VkImage), 用 ASurfaceTransaction 直接交给 SurfaceFlinger, 不走被限帧的缓冲队列。
// 游戏自己照常送显到它的 swapchain (被盖在下面), 节奏不受层影响。
//
// 依赖: libandroid 的 ASurfaceControl/ASurfaceTransaction (API 29+, dlsym), libnativewindow 的 AHardwareBuffer,
// device 扩展 VK_ANDROID_external_memory_android_hardware_buffer (CreateDevice 时由层加上)。
// 没有 android 头文件的宿主构建里本模块全部是空操作。
#pragma once
#include <vulkan/vulkan.h>
#include <string>
#include <vector>

struct ANativeWindow;

// interp_backend=sc 时 CreateDevice 前要加的 device 扩展 (只返回 available 里有、already 里没有的)
std::vector<const char*> sc_wanted_device_exts(const std::vector<std::string>& available,
                                              const std::vector<std::string>& already);
bool sc_requested();                 // debug.knobs.interp_backend == "sc"
void sc_on_window(ANativeWindow* w); // 游戏的窗口 (vkCreateAndroidSurfaceKHR 时)
// device 建好后: 取函数, 记下扩展是否真的开了
void sc_on_device(VkDevice dev, VkPhysicalDevice phys, PFN_vkGetDeviceProcAddr gdpa,
                  PFN_vkGetPhysicalDeviceMemoryProperties gpmp, bool ext_enabled);
bool sc_ready(VkDevice dev);         // 子图层与函数都齐了

// 一张层自己的显示缓冲 (AHardwareBuffer 导入的 VkImage), 宽高 = 游戏 swapchain
struct ScBuf {
    VkImage img = VK_NULL_HANDLE;
    void* ahb = nullptr;
    bool fresh = true;               // 还没被 SF 拿过 (首次用时布局为 UNDEFINED, 不做外部队列接收)
};
// 取下一张可写的缓冲 (环形, 容量 n); 尺寸变了重建。失败返回空。
ScBuf* sc_next_buffer(VkDevice dev, uint32_t w, uint32_t h, uint32_t ring);
// 录命令: 交给外部 (SF) 之前的屏障 (TRANSFER_DST → GENERAL, 队列族 → FOREIGN)
void sc_release_barrier(VkCommandBuffer cb, PFN_vkCmdPipelineBarrier bar, VkImage img, uint32_t family);
// 录命令: 从外部收回、准备写 (FOREIGN → 队列族, UNDEFINED → TRANSFER_DST)
void sc_acquire_barrier(VkCommandBuffer cb, PFN_vkCmdPipelineBarrier bar, VkImage img, uint32_t family, bool fresh);
// 层的命令提交之后: 导出一个 sync fd (这之前同队列的工作做完才 signal); 拿不到返回 -1 (调用方改为 CPU 等)
int sc_export_fence(VkDevice dev, VkQueue q);
// 依次把缓冲交给子图层, desired = CLOCK_MONOTONIC ns (0 = 尽快); fence_fd 由本函数接管 (-1 = 已写完)
void sc_show(VkDevice dev, const std::vector<std::pair<ScBuf*, int64_t>>& frames, uint32_t w, uint32_t h, uint32_t pre_transform, int fence_fd);
void sc_on_destroy_device(VkDevice dev);
// debug.knobs.interp_sc_grid=1 且已估出 vsync 网格时: phase = 最近一次 vsync (CLOCK_MONOTONIC ns), period = 周期; 否则 false
bool sc_vsync_grid(uint64_t* phase, uint64_t* period);
std::string sc_json();
