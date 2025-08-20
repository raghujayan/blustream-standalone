#pragma once

#include <memory>
#include <string>

// OpenGL and windowing headers
#ifdef __linux__
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
// Prevent X11 None macro from conflicting with HueSpace
#ifdef None
#undef None
#endif
#elif defined(_WIN32)
#include <GL/gl.h>
#include <windows.h>
#endif

namespace blustream {
namespace server {

/**
 * @brief OpenGL context manager for headless offscreen rendering
 * 
 * Creates an invisible OpenGL context suitable for server-side rendering
 * without requiring a display or GUI. Works with X11 on Linux and 
 * Windows OpenGL contexts on Windows.
 */
class OpenGLContext {
public:
    /**
     * @brief Configuration for OpenGL context creation
     */
    struct ContextConfig {
        int width = 1920;           ///< Render target width
        int height = 1080;          ///< Render target height
        int color_bits = 24;        ///< Color buffer bit depth
        int depth_bits = 24;        ///< Depth buffer bit depth
        int stencil_bits = 8;       ///< Stencil buffer bit depth
        int samples = 0;            ///< MSAA samples (0 = no MSAA)
        bool double_buffer = false; ///< Double buffering (not needed for offscreen)
        int gl_major = 4;           ///< OpenGL major version
        int gl_minor = 3;           ///< OpenGL minor version
        bool core_profile = true;   ///< Use core profile (vs compatibility)
    };

    OpenGLContext();
    ~OpenGLContext();

    /**
     * @brief Create headless OpenGL context
     * 
     * @param config Context configuration parameters
     * @return true if context created successfully
     */
    bool create_context(const ContextConfig& config);
    
    /**
     * @brief Create headless OpenGL context with default configuration
     * 
     * @return true if context created successfully
     */
    bool create_context();

    /**
     * @brief Make this context current for rendering
     * 
     * @return true if successful
     */
    bool make_current();

    /**
     * @brief Release the current context
     * 
     * @return true if successful
     */
    bool release_context();

    /**
     * @brief Destroy the OpenGL context and cleanup resources
     */
    void destroy();

    /**
     * @brief Check if context is valid and ready for rendering
     * 
     * @return true if context is valid
     */
    bool is_valid() const;

    /**
     * @brief Get context configuration
     * 
     * @return Current context configuration
     */
    const ContextConfig& get_config() const { return config_; }

    /**
     * @brief Get OpenGL version string
     * 
     * @return OpenGL version string
     */
    std::string get_gl_version() const;

    /**
     * @brief Get OpenGL renderer string
     * 
     * @return OpenGL renderer string
     */
    std::string get_gl_renderer() const;

private:
    ContextConfig config_;
    bool context_valid_ = false;

#ifdef __linux__
    Display* display_ = nullptr;
    GLXContext glx_context_ = nullptr;
    GLXPbuffer pbuffer_ = 0;  // Keep for compatibility
    GLXPixmap glx_pixmap_ = 0;  // Use GLXPixmap instead of pbuffer for Xvfb
    Pixmap pixmap_ = 0;
    XVisualInfo* visual_info_ = nullptr;
    GLXFBConfig fb_config_ = nullptr;
    
    bool create_x11_context();
    void cleanup_x11();
#elif defined(_WIN32)
    HGLRC gl_context_ = nullptr;
    HDC device_context_ = nullptr;
    HWND window_handle_ = nullptr;
    
    bool create_win32_context();
    void cleanup_win32();
#endif

    /**
     * @brief Initialize OpenGL function pointers and check capabilities
     * 
     * @return true if initialization successful
     */
    bool initialize_gl();
};

}  // namespace server
}  // namespace blustream