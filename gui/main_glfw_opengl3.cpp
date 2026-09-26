// Linux / macOS entry point: GLFW window + OpenGL 3 renderer for Dear ImGui.
// Adapted from Dear ImGui's example_glfw_opengl3 (MIT).

#include "app.hpp"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "theme.hpp"

#define GL_SILENCE_DEPRECATION
#include <GLFW/glfw3.h>

#include <cstdio>
#include <string>

static void glfwError(int error, const char* description) { std::fprintf(stderr, "GLFW error %d: %s\n", error, description); }

int main(int argc, char** argv) {
    glfwSetErrorCallback(glfwError);
    if (!glfwInit()) return 1;

#if defined(__APPLE__)
    const char* glslVersion = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
    const char* glslVersion = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

    const float scale = ImGui_ImplGlfw_GetContentScaleForMonitor(glfwGetPrimaryMonitor());
    GLFWwindow* window = glfwCreateWindow(static_cast<int>(1360 * scale), static_cast<int>(860 * scale), "OpenBooks",
                                          nullptr, nullptr);
    if (!window) return 1;
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    glfwSetWindowSizeLimits(window, 900, 600, GLFW_DONT_CARE, GLFW_DONT_CARE);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::StyleColorsLight();
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glslVersion);
    obgui::loadFonts();

    try {
        obgui::App app(argc > 1 ? argv[1] : "");
        std::string title;
        int busyFrames = 3;
        while (!glfwWindowShouldClose(window)) {
            // Sleep until there is input (the wait returns early), unless something is animating.
            if (busyFrames <= 0 && !app.wantsFrequentRedraw()) {
                const double start = glfwGetTime();
                glfwWaitEventsTimeout(0.5);
                busyFrames = glfwGetTime() - start < 0.45 ? 3 : 0;
            } else {
                glfwPollEvents();
                busyFrames = ImGui::IsAnyItemActive() ? 3 : busyFrames - 1;
            }
            if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0) {
                ImGui_ImplGlfw_Sleep(10);
                continue;
            }

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            app.frame();
            ImGui::Render();

            int width = 0;
            int height = 0;
            glfwGetFramebufferSize(window, &width, &height);
            glViewport(0, 0, width, height);
            const ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
            glClearColor(bg.x, bg.y, bg.z, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(window);

            if (app.windowTitle() != title) {
                title = app.windowTitle();
                glfwSetWindowTitle(window, title.c_str());
            }
            if (app.quitRequested()) glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "OpenBooks hit an unexpected error and has to close: %s\n", e.what());
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
