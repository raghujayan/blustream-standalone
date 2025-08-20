#include "blustream/server/opengl_context.h"
#include "blustream/common/logger.h"

#include <cstring>
#include <vector>

namespace blustream {
namespace server {

OpenGLContext::OpenGLContext() = default;

OpenGLContext::~OpenGLContext() {
    destroy();
}

bool OpenGLContext::create_context(const ContextConfig& config) {
    config_ = config;
    
    BLUSTREAM_LOG_INFO("🎨 Creating OpenGL context for headless rendering...");
    BLUSTREAM_LOG_INFO("   Resolution: " + std::to_string(config.width) + "x" + std::to_string(config.height));
    BLUSTREAM_LOG_INFO("   OpenGL: " + std::to_string(config.gl_major) + "." + std::to_string(config.gl_minor));
    
#ifdef __linux__
    return create_x11_context();
#elif defined(_WIN32)
    return create_win32_context();
#else
    BLUSTREAM_LOG_ERROR("Unsupported platform for OpenGL context creation");
    return false;
#endif
}

bool OpenGLContext::create_context() {
    ContextConfig default_config;
    return create_context(default_config);
}

bool OpenGLContext::make_current() {
    if (!context_valid_) {
        BLUSTREAM_LOG_ERROR("Cannot make invalid OpenGL context current");
        return false;
    }

#ifdef __linux__
    // Use GLXPixmap if available, fall back to pbuffer
    if (glx_pixmap_) {
        if (!glXMakeCurrent(display_, glx_pixmap_, glx_context_)) {
            BLUSTREAM_LOG_ERROR("Failed to make GLX context current with pixmap");
            return false;
        }
    } else if (pbuffer_) {
        if (!glXMakeContextCurrent(display_, pbuffer_, pbuffer_, glx_context_)) {
            BLUSTREAM_LOG_ERROR("Failed to make GLX context current with pbuffer");
            return false;
        }
    } else {
        BLUSTREAM_LOG_ERROR("No drawable available for GLX context");
        return false;
    }
#elif defined(_WIN32)
    if (!wglMakeCurrent(device_context_, gl_context_)) {
        BLUSTREAM_LOG_ERROR("Failed to make WGL context current");
        return false;
    }
#endif

    return true;
}

bool OpenGLContext::release_context() {
#ifdef __linux__
    return glXMakeContextCurrent(display_, 0, 0, nullptr);
#elif defined(_WIN32)
    return wglMakeCurrent(nullptr, nullptr);
#else
    return false;
#endif
}

void OpenGLContext::destroy() {
    if (!context_valid_) return;
    
    BLUSTREAM_LOG_INFO("🧹 Destroying OpenGL context...");
    
    release_context();
    
#ifdef __linux__
    cleanup_x11();
#elif defined(_WIN32)
    cleanup_win32();
#endif
    
    context_valid_ = false;
}

bool OpenGLContext::is_valid() const {
    return context_valid_;
}

std::string OpenGLContext::get_gl_version() const {
    if (!context_valid_) return "No context";
    
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    return version ? std::string(version) : "Unknown";
}

std::string OpenGLContext::get_gl_renderer() const {
    if (!context_valid_) return "No context";
    
    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    return renderer ? std::string(renderer) : "Unknown";
}

#ifdef __linux__
bool OpenGLContext::create_x11_context() {
    // Open X11 display
    display_ = XOpenDisplay(":0");  // Use DISPLAY environment variable or :0
    if (!display_) {
        display_ = XOpenDisplay(nullptr);  // Try default display
        if (!display_) {
            BLUSTREAM_LOG_ERROR("Failed to open X11 display - ensure DISPLAY is set or Xvfb is running");
            return false;
        }
    }
    
    BLUSTREAM_LOG_INFO("✅ X11 display opened successfully");
    
    // Configure framebuffer attributes for off-screen rendering
    int fb_attribs[] = {
        GLX_RENDER_TYPE, GLX_RGBA_BIT,
        GLX_DRAWABLE_TYPE, GLX_PBUFFER_BIT,
        GLX_RED_SIZE, 8,
        GLX_GREEN_SIZE, 8,
        GLX_BLUE_SIZE, 8,
        GLX_ALPHA_SIZE, 8,
        GLX_DEPTH_SIZE, config_.depth_bits,
        GLX_STENCIL_SIZE, config_.stencil_bits,
        GLX_DOUBLEBUFFER, config_.double_buffer ? True : False,
        0
    };
    
    // Get framebuffer configurations
    int num_configs = 0;
    GLXFBConfig* fb_configs = glXChooseFBConfig(display_, DefaultScreen(display_), 
                                                fb_attribs, &num_configs);
    if (!fb_configs || num_configs == 0) {
        BLUSTREAM_LOG_ERROR("No suitable GLX framebuffer configurations found");
        XCloseDisplay(display_);
        display_ = nullptr;
        return false;
    }
    
    fb_config_ = fb_configs[0];  // Use first suitable config
    XFree(fb_configs);
    
    BLUSTREAM_LOG_INFO("✅ GLX framebuffer configuration selected");
    
    // Create OpenGL context
    int context_attribs[] = {
        GLX_CONTEXT_MAJOR_VERSION_ARB, config_.gl_major,
        GLX_CONTEXT_MINOR_VERSION_ARB, config_.gl_minor,
        GLX_CONTEXT_PROFILE_MASK_ARB, config_.core_profile ? GLX_CONTEXT_CORE_PROFILE_BIT_ARB : GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB,
        0
    };
    
    // Try to get the extension function first
    typedef GLXContext (*glXCreateContextAttribsARBProc)(Display*, GLXFBConfig, GLXContext, Bool, const int*);
    glXCreateContextAttribsARBProc glXCreateContextAttribsARB = 
        (glXCreateContextAttribsARBProc) glXGetProcAddressARB((const GLubyte*)"glXCreateContextAttribsARB");
    
    if (glXCreateContextAttribsARB) {
        glx_context_ = glXCreateContextAttribsARB(display_, fb_config_, nullptr, True, context_attribs);
    }
    
    if (!glx_context_) {
        BLUSTREAM_LOG_WARN("Failed to create OpenGL " + std::to_string(config_.gl_major) + "." + std::to_string(config_.gl_minor) + " context, trying legacy...");
        glx_context_ = glXCreateNewContext(display_, fb_config_, GLX_RGBA_TYPE, nullptr, True);
    }
    
    if (!glx_context_) {
        BLUSTREAM_LOG_ERROR("Failed to create GLX context");
        XCloseDisplay(display_);
        display_ = nullptr;
        return false;
    }
    
    BLUSTREAM_LOG_INFO("✅ GLX context created successfully");
    
    // Create pixmap for off-screen rendering (more compatible with Xvfb than pbuffer)
    // First get the visual from the FBConfig
    visual_info_ = glXGetVisualFromFBConfig(display_, fb_config_);
    if (!visual_info_) {
        BLUSTREAM_LOG_ERROR("Failed to get visual from FBConfig");
        glXDestroyContext(display_, glx_context_);
        XCloseDisplay(display_);
        display_ = nullptr;
        glx_context_ = nullptr;
        return false;
    }
    
    // Create a pixmap for offscreen rendering
    Window root = DefaultRootWindow(display_);
    pixmap_ = XCreatePixmap(display_, root, config_.width, config_.height, visual_info_->depth);
    if (!pixmap_) {
        BLUSTREAM_LOG_ERROR("Failed to create X11 Pixmap");
        XFree(visual_info_);
        glXDestroyContext(display_, glx_context_);
        XCloseDisplay(display_);
        display_ = nullptr;
        glx_context_ = nullptr;
        visual_info_ = nullptr;
        return false;
    }
    
    // Create GLX pixmap from the X11 pixmap
    glx_pixmap_ = glXCreateGLXPixmap(display_, visual_info_, pixmap_);
    if (!glx_pixmap_) {
        BLUSTREAM_LOG_ERROR("Failed to create GLX pixmap");
        XFreePixmap(display_, pixmap_);
        XFree(visual_info_);
        glXDestroyContext(display_, glx_context_);
        XCloseDisplay(display_);
        display_ = nullptr;
        glx_context_ = nullptr;
        visual_info_ = nullptr;
        pixmap_ = 0;
        return false;
    }
    
    BLUSTREAM_LOG_INFO("✅ GLX pixmap created: " + std::to_string(config_.width) + "x" + std::to_string(config_.height));
    
    // Make context current using GLXPixmap (more compatible with Xvfb)
    if (!glXMakeCurrent(display_, glx_pixmap_, glx_context_)) {
        BLUSTREAM_LOG_ERROR("Failed to make GLX context current during initialization");
        cleanup_x11();
        return false;
    }
    
    // Now initialize OpenGL
    if (!initialize_gl()) {
        cleanup_x11();
        return false;
    }
    
    context_valid_ = true;
    return true;
}

void OpenGLContext::cleanup_x11() {
    if (glx_pixmap_) {
        glXDestroyGLXPixmap(display_, glx_pixmap_);
        glx_pixmap_ = 0;
    }
    
    if (pixmap_) {
        XFreePixmap(display_, pixmap_);
        pixmap_ = 0;
    }
    
    if (visual_info_) {
        XFree(visual_info_);
        visual_info_ = nullptr;
    }
    
    if (pbuffer_) {
        glXDestroyPbuffer(display_, pbuffer_);
        pbuffer_ = 0;
    }
    
    if (glx_context_) {
        glXDestroyContext(display_, glx_context_);
        glx_context_ = nullptr;
    }
    
    if (display_) {
        XCloseDisplay(display_);
        display_ = nullptr;
    }
}
#endif

#ifdef _WIN32
bool OpenGLContext::create_win32_context() {
    // Create invisible window for context
    WNDCLASS wc = {};
    wc.lpfnWndProc = DefWindowProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"BluStreamOffscreenWindow";
    
    if (!RegisterClass(&wc)) {
        BLUSTREAM_LOG_ERROR("Failed to register window class");
        return false;
    }
    
    window_handle_ = CreateWindow(L"BluStreamOffscreenWindow", L"BluStream Offscreen",
                                  WS_OVERLAPPEDWINDOW, 0, 0, config_.width, config_.height,
                                  nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (!window_handle_) {
        BLUSTREAM_LOG_ERROR("Failed to create window");
        return false;
    }
    
    device_context_ = GetDC(window_handle_);
    if (!device_context_) {
        BLUSTREAM_LOG_ERROR("Failed to get device context");
        return false;
    }
    
    // Set pixel format
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(PIXELFORMATDESCRIPTOR);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = config_.color_bits;
    pfd.cDepthBits = config_.depth_bits;
    pfd.cStencilBits = config_.stencil_bits;
    
    int pixel_format = ChoosePixelFormat(device_context_, &pfd);
    if (!pixel_format || !SetPixelFormat(device_context_, pixel_format, &pfd)) {
        BLUSTREAM_LOG_ERROR("Failed to set pixel format");
        cleanup_win32();
        return false;
    }
    
    // Create OpenGL context
    gl_context_ = wglCreateContext(device_context_);
    if (!gl_context_) {
        BLUSTREAM_LOG_ERROR("Failed to create OpenGL context");
        cleanup_win32();
        return false;
    }
    
    // Make context current and initialize
    if (!make_current()) {
        cleanup_win32();
        return false;
    }
    
    if (!initialize_gl()) {
        cleanup_win32();
        return false;
    }
    
    context_valid_ = true;
    return true;
}

void OpenGLContext::cleanup_win32() {
    if (gl_context_) {
        wglDeleteContext(gl_context_);
        gl_context_ = nullptr;
    }
    
    if (device_context_) {
        ReleaseDC(window_handle_, device_context_);
        device_context_ = nullptr;
    }
    
    if (window_handle_) {
        DestroyWindow(window_handle_);
        window_handle_ = nullptr;
    }
}
#endif

bool OpenGLContext::initialize_gl() {
    // Check OpenGL version
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const char* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    
    if (!version || !renderer || !vendor) {
        BLUSTREAM_LOG_ERROR("Failed to query OpenGL strings");
        return false;
    }
    
    BLUSTREAM_LOG_INFO("🎮 OpenGL Context Initialized:");
    BLUSTREAM_LOG_INFO("   Version: " + std::string(version));
    BLUSTREAM_LOG_INFO("   Renderer: " + std::string(renderer));
    BLUSTREAM_LOG_INFO("   Vendor: " + std::string(vendor));
    
    // Check for required OpenGL version
    int major, minor;
    if (sscanf(version, "%d.%d", &major, &minor) != 2) {
        BLUSTREAM_LOG_ERROR("Failed to parse OpenGL version");
        return false;
    }
    
    if (major < config_.gl_major || (major == config_.gl_major && minor < config_.gl_minor)) {
        BLUSTREAM_LOG_ERROR("OpenGL " + std::to_string(config_.gl_major) + "." + std::to_string(config_.gl_minor) + " required, got " + std::to_string(major) + "." + std::to_string(minor));
        return false;
    }
    
    // Set up initial OpenGL state
    glViewport(0, 0, config_.width, config_.height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClearDepth(1.0f);
    glEnable(GL_DEPTH_TEST);
    
    // Check for OpenGL errors
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        BLUSTREAM_LOG_ERROR("OpenGL error during initialization: " + std::to_string(error));
        return false;
    }
    
    BLUSTREAM_LOG_INFO("✅ OpenGL context ready for rendering");
    return true;
}

}  // namespace server
}  // namespace blustream