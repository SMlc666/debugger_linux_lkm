#include <jni.h>

#include <android/native_window.h>
#include <android/native_window_jni.h>

#include <string>
#include <vector>

#include "nativeui/imgui_workspace_renderer.h"

namespace {

using lkmdbg::nativeui::ImGuiWorkspaceRenderer;
using lkmdbg::nativeui::WorkspaceLabels;

ImGuiWorkspaceRenderer *from_handle(jlong handle)
{
	return reinterpret_cast<ImGuiWorkspaceRenderer *>(handle);
}

std::string jstring_to_string(JNIEnv *env, jstring value)
{
	if (!value)
		return {};
	const char *chars = env->GetStringUTFChars(value, nullptr);
	std::string out = chars ? chars : "";
	if (chars)
		env->ReleaseStringUTFChars(value, chars);
	return out;
}

} // namespace

extern "C" JNIEXPORT jlong JNICALL
Java_com_smlc666_lkmdbg_nativeui_NativeWorkspaceBridge_nativeCreateRenderer(
	JNIEnv *, jobject)
{
	return reinterpret_cast<jlong>(new ImGuiWorkspaceRenderer());
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
	ImGuiWorkspaceRenderer *renderer = from_handle(handle);
	ANativeWindow *window = nullptr;

	if (!renderer)
		return;
	if (surface)
		window = ANativeWindow_fromSurface(env, surface);
	renderer->SetSurface(window);
	if (window)
		ANativeWindow_release(window);
}

extern "C" JNIEXPORT void JNICALL
Java_com_smlc666_lkmdbg_nativeui_NativeWorkspaceBridge_nativeResize(
	JNIEnv *, jobject, jlong handle, jint width, jint height, jfloat density)
{
	ImGuiWorkspaceRenderer *renderer = from_handle(handle);
	if (!renderer)
		return;
	renderer->Resize(width, height, density);
}

extern "C" JNIEXPORT void JNICALL
Java_com_smlc666_lkmdbg_nativeui_NativeWorkspaceBridge_nativeUpdateState(
	JNIEnv *, jobject, jlong handle, jboolean expanded, jboolean connected,
	jboolean session_open, jint hook_active, jint process_count,
	jint thread_count, jint event_count)
{
	ImGuiWorkspaceRenderer *renderer = from_handle(handle);
	if (!renderer)
		return;
	renderer->UpdateState(expanded, connected, session_open, hook_active,
			      process_count, thread_count, event_count);
}

extern "C" JNIEXPORT void JNICALL
Java_com_smlc666_lkmdbg_nativeui_NativeWorkspaceBridge_nativeUpdateStrings(
	JNIEnv *env, jobject, jlong handle, jstring title, jstring session,
	jstring processes, jstring memory, jstring threads, jstring events,
	jstring connected, jstring session_open, jstring hook,
	jstring process_count, jstring thread_count, jstring event_count)
{
	ImGuiWorkspaceRenderer *renderer = from_handle(handle);
	WorkspaceLabels labels;

	if (!renderer)
		return;
	labels.title = jstring_to_string(env, title);
	labels.session = jstring_to_string(env, session);
	labels.processes = jstring_to_string(env, processes);
	labels.memory = jstring_to_string(env, memory);
	labels.threads = jstring_to_string(env, threads);
	labels.events = jstring_to_string(env, events);
	labels.connected = jstring_to_string(env, connected);
	labels.session_open = jstring_to_string(env, session_open);
	labels.hook = jstring_to_string(env, hook);
	labels.process_count = jstring_to_string(env, process_count);
	labels.thread_count = jstring_to_string(env, thread_count);
	labels.event_count = jstring_to_string(env, event_count);
	renderer->UpdateLabels(labels);
}

extern "C" JNIEXPORT void JNICALL
Java_com_smlc666_lkmdbg_nativeui_NativeWorkspaceBridge_nativeUpdateFontPaths(
	JNIEnv *env, jobject, jlong handle, jobjectArray font_paths)
{
	ImGuiWorkspaceRenderer *renderer = from_handle(handle);
	std::vector<std::string> paths;

	if (!renderer || !font_paths)
		return;
	jsize count = env->GetArrayLength(font_paths);
	paths.reserve(static_cast<size_t>(count));
	for (jsize i = 0; i < count; ++i) {
		jstring item = static_cast<jstring>(env->GetObjectArrayElement(font_paths, i));
		paths.push_back(jstring_to_string(env, item));
		env->DeleteLocalRef(item);
	}
	renderer->UpdateFontPaths(std::move(paths));
}

extern "C" JNIEXPORT void JNICALL
Java_com_smlc666_lkmdbg_nativeui_NativeWorkspaceBridge_nativeOnTouch(
	JNIEnv *, jobject, jlong handle, jint action, jfloat x, jfloat y)
{
	ImGuiWorkspaceRenderer *renderer = from_handle(handle);
	if (!renderer)
		return;
	renderer->OnTouch(action, x, y);
}

extern "C" JNIEXPORT void JNICALL
Java_com_smlc666_lkmdbg_nativeui_NativeWorkspaceBridge_nativeRender(
	JNIEnv *, jobject, jlong handle)
{
	ImGuiWorkspaceRenderer *renderer = from_handle(handle);
	if (!renderer)
		return;
	renderer->Render();
}
