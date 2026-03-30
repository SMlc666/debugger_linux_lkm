#include <jni.h>

#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>

#include <algorithm>
#include <cstdint>
#include <mutex>

namespace {

struct Color {
	uint8_t r;
	uint8_t g;
	uint8_t b;
	uint8_t a;
};

static uint32_t pack_color(const Color &color)
{
	return (static_cast<uint32_t>(color.a) << 24) |
	       (static_cast<uint32_t>(color.r) << 16) |
	       (static_cast<uint32_t>(color.g) << 8) |
	       static_cast<uint32_t>(color.b);
}

class NativeWorkspaceRenderer {
public:
	void set_surface(ANativeWindow *window)
	{
		std::scoped_lock lock(mutex_);
		if (window_ == window)
			return;
		if (window_)
			ANativeWindow_release(window_);
		window_ = window;
		if (window_)
			ANativeWindow_acquire(window_);
	}

	void resize(int width, int height, float density)
	{
		std::scoped_lock lock(mutex_);
		width_ = width;
		height_ = height;
		density_ = density;
	}

	void update_state(bool expanded, bool connected, bool session_open,
			  int hook_active, int process_count, int thread_count,
			  int event_count)
	{
		std::scoped_lock lock(mutex_);
		expanded_ = expanded;
		connected_ = connected;
		session_open_ = session_open;
		hook_active_ = hook_active;
		process_count_ = process_count;
		thread_count_ = thread_count;
		event_count_ = event_count;
	}

	void render()
	{
		std::scoped_lock lock(mutex_);
		ANativeWindow_Buffer buffer;
		if (!window_)
			return;
		if (ANativeWindow_setBuffersGeometry(window_, width_, height_,
						 WINDOW_FORMAT_RGBA_8888) != 0)
			return;
		if (ANativeWindow_lock(window_, &buffer, nullptr) != 0)
			return;
		clear_buffer(buffer, expanded_ ? Color{8, 18, 26, 245} : Color{0, 0, 0, 0});
		if (expanded_)
			draw_workspace(buffer);
		else
			draw_ball(buffer);
		ANativeWindow_unlockAndPost(window_);
	}

private:
	static void fill_rect(ANativeWindow_Buffer &buffer, int x, int y, int width,
			      int height, const Color &color)
	{
		const int x0 = std::max(0, x);
		const int y0 = std::max(0, y);
		const int x1 = std::min(buffer.width, x + width);
		const int y1 = std::min(buffer.height, y + height);
		uint32_t *pixels = static_cast<uint32_t *>(buffer.bits);
		const uint32_t packed = pack_color(color);

		for (int row = y0; row < y1; ++row) {
			uint32_t *line = pixels + row * buffer.stride;
			for (int col = x0; col < x1; ++col)
				line[col] = packed;
		}
	}

	static void fill_circle(ANativeWindow_Buffer &buffer, int center_x,
				int center_y, int radius, const Color &color)
	{
		uint32_t *pixels = static_cast<uint32_t *>(buffer.bits);
		const uint32_t packed = pack_color(color);
		const int radius_sq = radius * radius;

		for (int y = std::max(0, center_y - radius);
		     y < std::min(buffer.height, center_y + radius); ++y) {
			uint32_t *line = pixels + y * buffer.stride;
			for (int x = std::max(0, center_x - radius);
			     x < std::min(buffer.width, center_x + radius); ++x) {
				const int dx = x - center_x;
				const int dy = y - center_y;
				if (dx * dx + dy * dy <= radius_sq)
					line[x] = packed;
			}
		}
	}

	static void clear_buffer(ANativeWindow_Buffer &buffer, const Color &color)
	{
		fill_rect(buffer, 0, 0, buffer.width, buffer.height, color);
	}

	void draw_workspace(ANativeWindow_Buffer &buffer)
	{
		const int pad = static_cast<int>(14.0f * density_);
		const int rail_width = static_cast<int>(92.0f * density_);
		const Color panel = {14, 31, 43, 255};
		const Color accent = connected_ ? Color{73, 216, 210, 255} : Color{89, 111, 128, 255};
		const Color muted = {24, 49, 63, 255};
		const int header_h = static_cast<int>(88.0f * density_);

		fill_rect(buffer, pad, pad, buffer.width - pad * 2, header_h, panel);
		fill_rect(buffer, pad, header_h + pad * 2, rail_width, buffer.height - header_h - pad * 3, panel);
		fill_rect(buffer, rail_width + pad * 2, header_h + pad * 2,
			  buffer.width - rail_width - pad * 3,
			  buffer.height - header_h - pad * 3, panel);

		for (int i = 0; i < 5; ++i) {
			fill_rect(buffer, pad + static_cast<int>(12.0f * density_),
				  header_h + pad * 3 + i * static_cast<int>(42.0f * density_),
				  rail_width - static_cast<int>(24.0f * density_),
				  static_cast<int>(28.0f * density_),
				  i == 0 ? accent : muted);
		}

		const int main_x = rail_width + pad * 3;
		const int main_width = buffer.width - main_x - pad;
		fill_rect(buffer, main_x, header_h + pad * 3,
			  main_width, static_cast<int>(96.0f * density_), muted);
		fill_rect(buffer, main_x, header_h + pad * 4 + static_cast<int>(96.0f * density_),
			  main_width, buffer.height - header_h - pad * 6 - static_cast<int>(96.0f * density_),
			  Color{9, 22, 31, 255});

		for (int i = 0; i < 6; ++i) {
			const int row_y = header_h + pad * 5 + static_cast<int>(118.0f * density_) +
					  i * static_cast<int>(34.0f * density_);
			fill_rect(buffer, main_x + static_cast<int>(18.0f * density_), row_y,
				  main_width - static_cast<int>(36.0f * density_),
				  static_cast<int>(18.0f * density_),
				  i < process_count_ ? accent : muted);
		}

		const int badge_w = static_cast<int>(84.0f * density_);
		fill_rect(buffer, main_x + static_cast<int>(18.0f * density_),
			  header_h + pad * 3 + static_cast<int>(18.0f * density_),
			  badge_w, static_cast<int>(24.0f * density_), accent);
		fill_rect(buffer, main_x + static_cast<int>(18.0f * density_) + badge_w + pad,
			  header_h + pad * 3 + static_cast<int>(18.0f * density_),
			  badge_w, static_cast<int>(24.0f * density_),
			  session_open_ ? accent : muted);
		fill_rect(buffer, main_x + static_cast<int>(18.0f * density_) + (badge_w + pad) * 2,
			  header_h + pad * 3 + static_cast<int>(18.0f * density_),
			  badge_w, static_cast<int>(24.0f * density_),
			  hook_active_ > 0 ? accent : muted);
	}

	void draw_ball(ANativeWindow_Buffer &buffer)
	{
		const int radius = std::min(buffer.width, buffer.height) / 2 - static_cast<int>(4.0f * density_);
		const int cx = buffer.width / 2;
		const int cy = buffer.height / 2;

		fill_circle(buffer, cx, cy, radius, Color{73, 216, 210, 255});
		fill_circle(buffer, cx, cy, radius - static_cast<int>(10.0f * density_),
			    Color{12, 25, 34, 255});
		fill_circle(buffer, cx, cy, radius - static_cast<int>(22.0f * density_),
			    Color{73, 216, 210, 255});
	}

	std::mutex mutex_;
	ANativeWindow *window_ = nullptr;
	int width_ = 1;
	int height_ = 1;
	float density_ = 1.0f;
	bool expanded_ = false;
	bool connected_ = false;
	bool session_open_ = false;
	int hook_active_ = 0;
	int process_count_ = 0;
	int thread_count_ = 0;
	int event_count_ = 0;
};

static NativeWorkspaceRenderer *from_handle(jlong handle)
{
	return reinterpret_cast<NativeWorkspaceRenderer *>(handle);
}

} // namespace

extern "C" JNIEXPORT jlong JNICALL
Java_com_smlc666_lkmdbg_nativeui_NativeWorkspaceBridge_nativeCreateRenderer(
	JNIEnv *, jobject)
{
	return reinterpret_cast<jlong>(new NativeWorkspaceRenderer());
}

extern "C" JNIEXPORT void JNICALL
Java_com_smlc666_lkmdbg_nativeui_NativeWorkspaceBridge_nativeDestroyRenderer(
	JNIEnv *, jobject, jlong handle)
{
	delete from_handle(handle);
}

extern "C" JNIEXPORT void JNICALL
Java_com_smlc666_lkmdbg_nativeui_NativeWorkspaceBridge_nativeSetSurface(
	JNIEnv *env, jobject, jlong handle, jobject surface)
{
	NativeWorkspaceRenderer *renderer = from_handle(handle);
	ANativeWindow *window = nullptr;

	if (!renderer)
		return;
	if (surface)
		window = ANativeWindow_fromSurface(env, surface);
	renderer->set_surface(window);
	if (window)
		ANativeWindow_release(window);
}

extern "C" JNIEXPORT void JNICALL
Java_com_smlc666_lkmdbg_nativeui_NativeWorkspaceBridge_nativeResize(
	JNIEnv *, jobject, jlong handle, jint width, jint height, jfloat density)
{
	NativeWorkspaceRenderer *renderer = from_handle(handle);

	if (!renderer)
		return;
	renderer->resize(width, height, density);
}

extern "C" JNIEXPORT void JNICALL
Java_com_smlc666_lkmdbg_nativeui_NativeWorkspaceBridge_nativeUpdateState(
	JNIEnv *, jobject, jlong handle, jboolean expanded, jboolean connected,
	jboolean session_open, jint hook_active, jint process_count,
	jint thread_count, jint event_count)
{
	NativeWorkspaceRenderer *renderer = from_handle(handle);

	if (!renderer)
		return;
	renderer->update_state(expanded, connected, session_open, hook_active,
			       process_count, thread_count, event_count);
}

extern "C" JNIEXPORT void JNICALL
Java_com_smlc666_lkmdbg_nativeui_NativeWorkspaceBridge_nativeRender(
	JNIEnv *, jobject, jlong handle)
{
	NativeWorkspaceRenderer *renderer = from_handle(handle);

	if (!renderer)
		return;
	renderer->render();
}
