#pragma once

#include <EGL/egl.h>
#include <android/native_window.h>

namespace lkmdbg::nativeui {

class EglWindowContext {
public:
	EglWindowContext() = default;
	~EglWindowContext();

	EglWindowContext(const EglWindowContext &) = delete;
	EglWindowContext &operator=(const EglWindowContext &) = delete;

	void SetWindow(ANativeWindow *window);
	void Resize(int width, int height);
	bool EnsureCurrent();
	void SwapBuffers();
	void Shutdown();

	int width() const { return width_; }
	int height() const { return height_; }

private:
	void DestroySurface();

	ANativeWindow *window_ = nullptr;
	EGLDisplay display_ = EGL_NO_DISPLAY;
	EGLSurface surface_ = EGL_NO_SURFACE;
	EGLContext context_ = EGL_NO_CONTEXT;
	EGLConfig config_ = nullptr;
	int width_ = 1;
	int height_ = 1;
};

} // namespace lkmdbg::nativeui
