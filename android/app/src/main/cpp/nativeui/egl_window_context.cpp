#include "nativeui/egl_window_context.h"

namespace lkmdbg::nativeui {

EglWindowContext::~EglWindowContext()
{
	Shutdown();
	if (window_)
		ANativeWindow_release(window_);
}

void EglWindowContext::SetWindow(ANativeWindow *window)
{
	if (window_ == window)
		return;
	DestroySurface();
	if (window_)
		ANativeWindow_release(window_);
	window_ = window;
	if (window_)
		ANativeWindow_acquire(window_);
}

void EglWindowContext::Resize(int width, int height)
{
	width_ = width > 0 ? width : 1;
	height_ = height > 0 ? height : 1;
}

bool EglWindowContext::EnsureCurrent()
{
	if (!window_)
		return false;

	if (display_ == EGL_NO_DISPLAY) {
		display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
		if (display_ == EGL_NO_DISPLAY)
			return false;
		if (!eglInitialize(display_, nullptr, nullptr))
			return false;

		const EGLint config_attribs[] = {
			EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
			EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
			EGL_RED_SIZE, 8,
			EGL_GREEN_SIZE, 8,
			EGL_BLUE_SIZE, 8,
			EGL_ALPHA_SIZE, 8,
			EGL_NONE,
		};
		EGLint num_configs = 0;
		if (!eglChooseConfig(display_, config_attribs, &config_, 1, &num_configs) || num_configs == 0)
			return false;

		const EGLint context_attribs[] = {
			EGL_CONTEXT_CLIENT_VERSION, 3,
			EGL_NONE,
		};
		context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, context_attribs);
		if (context_ == EGL_NO_CONTEXT)
			return false;
	}

	if (surface_ == EGL_NO_SURFACE) {
		surface_ = eglCreateWindowSurface(display_, config_, window_, nullptr);
		if (surface_ == EGL_NO_SURFACE)
			return false;
	}

	return eglMakeCurrent(display_, surface_, surface_, context_) == EGL_TRUE;
}

void EglWindowContext::SwapBuffers()
{
	if (display_ != EGL_NO_DISPLAY && surface_ != EGL_NO_SURFACE)
		eglSwapBuffers(display_, surface_);
}

void EglWindowContext::Shutdown()
{
	DestroySurface();
	if (display_ != EGL_NO_DISPLAY && context_ != EGL_NO_CONTEXT) {
		eglDestroyContext(display_, context_);
		context_ = EGL_NO_CONTEXT;
	}
	if (display_ != EGL_NO_DISPLAY) {
		eglTerminate(display_);
		display_ = EGL_NO_DISPLAY;
	}
}

void EglWindowContext::DestroySurface()
{
	if (display_ != EGL_NO_DISPLAY && surface_ != EGL_NO_SURFACE) {
		eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		eglDestroySurface(display_, surface_);
		surface_ = EGL_NO_SURFACE;
	}
}

} // namespace lkmdbg::nativeui
