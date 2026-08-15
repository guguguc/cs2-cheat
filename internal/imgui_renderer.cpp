#include "imgui_renderer.h"

#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"
#include "logger.h"

bool ImGuiRenderer::init(VkInstance instance, VkPhysicalDevice phys, VkDevice device,
                         std::uint32_t queue_family, VkQueue queue, VkDescriptorPool pool,
                         VkRenderPass rp, std::uint32_t image_count, LoaderFn loader) {
    instance_ = instance;
    phys_ = phys;
    device_ = device;
    queue_family_ = queue_family;
    pool_ = pool;
    rp_ = rp;
    image_count_ = image_count;

    // The overlay is re-initialized every time the game recreates its
    // swapchain (resolution/fullscreen toggle). ImGui must only be created
    // ONCE - recreating the context and reloading fonts froze the game on
    // resolution switches.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    apply_theme();
    // The game hides the OS cursor (FPS), so ImGui must draw its own so the
    // menu is usable.
    ImGui::GetIO().MouseDrawCursor = true;
    ImGui::GetIO().IniFilename = "/tmp/cs2_internal_imgui.ini";
    if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, loader, nullptr)) {
        Logger::instance().error("imgui: ImGui_ImplVulkan_LoadFunctions failed\n");
        return false;
    }
    ImGui_ImplVulkan_InitInfo info = {};
    info.ApiVersion = VK_API_VERSION_1_3;
    info.Instance = instance;
    info.PhysicalDevice = phys;
    info.Device = device;
    info.QueueFamily = queue_family;
    info.Queue = queue;
    info.DescriptorPool = pool;
    info.MinImageCount = 2;
    info.ImageCount = image_count;
    info.PipelineInfoMain.RenderPass = rp;
    info.PipelineInfoMain.Subpass = 0;
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.UseDynamicRendering = false;
    if (!ImGui_ImplVulkan_Init(&info)) {
        Logger::instance().error("imgui: ImGui_ImplVulkan_Init failed\n");
        return false;
    }
    return true;
}

void ImGuiRenderer::shutdown() {
    if (device_ != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_Shutdown();
        device_ = VK_NULL_HANDLE;
    }
}

ImGuiIO& ImGuiRenderer::io() const {
    return ImGui::GetIO();
}

void ImGuiRenderer::new_frame() const {
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
}

void ImGuiRenderer::render_frame(VkCommandBuffer cmd) const {
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
}

void ImGuiRenderer::apply_theme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 10.f;
    st.ChildRounding = 6.f;
    st.FrameRounding = 6.f;
    st.GrabRounding = 6.f;
    st.PopupRounding = 8.f;
    st.ScrollbarRounding = 6.f;
    st.WindowBorderSize = 2.f;   // thicker border so the shadow reads
    st.FrameBorderSize = 0.f;
    st.WindowPadding = ImVec2(14.f, 12.f);
    st.FramePadding = ImVec2(8.f, 5.f);
    st.ItemSpacing = ImVec2(8.f, 7.f);

    ImVec4* c = st.Colors;
    const ImVec4 bg(0.0f, 0.0f, 0.0f, 1.0f);  // opaque black
    const ImVec4 green(0.25f, 0.95f, 0.60f, 1.f);
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.11f, 0.16f, 0.50f);
    c[ImGuiCol_PopupBg] = bg;
    c[ImGuiCol_Border] = ImVec4(0.25f, 0.95f, 0.60f, 0.55f);  // green glow border
    c[ImGuiCol_BorderShadow] = ImVec4(0.f, 0.f, 0.f, 0.60f);   // soft dark edge
    c[ImGuiCol_FrameBg] = ImVec4(1.f, 1.f, 1.f, 0.07f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(1.f, 1.f, 1.f, 0.12f);
    c[ImGuiCol_FrameBgActive] = ImVec4(green.x, green.y, green.z, 0.18f);
    c[ImGuiCol_CheckMark] = green;
    c[ImGuiCol_SliderGrab] = ImVec4(green.x, green.y, green.z, 0.9f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.35f, 1.f, 0.70f, 1.f);
    c[ImGuiCol_Header] = ImVec4(green.x, green.y, green.z, 0.15f);
    c[ImGuiCol_HeaderHovered] = ImVec4(green.x, green.y, green.z, 0.25f);
    c[ImGuiCol_HeaderActive] = ImVec4(green.x, green.y, green.z, 0.35f);
    c[ImGuiCol_Text] = ImVec4(0.90f, 0.92f, 0.95f, 1.f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.58f, 0.65f, 1.f);
    c[ImGuiCol_Button] = ImVec4(green.x, green.y, green.z, 0.15f);
    c[ImGuiCol_ButtonHovered] = ImVec4(green.x, green.y, green.z, 0.30f);
    c[ImGuiCol_ButtonActive] = ImVec4(green.x, green.y, green.z, 0.45f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.02f, 0.02f, 0.04f, 0.60f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(1.f, 1.f, 1.f, 0.25f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1.f, 1.f, 1.f, 0.35f);
    c[ImGuiCol_ScrollbarGrabActive] = green;
}
