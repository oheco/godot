// Godot Engine contributors. SPDX-License-Identifier: MIT
// Real ConsumerSurface presentation using the selected signed engine library.
// This uses the normal OS/Main bootstrap, without ArkTS, HAP or UIAbility.
#include "main/main.h"

#include "platform/openharmony/display_server_openharmony.h"
#include "platform/openharmony/engine_host_openharmony.h"
#include "platform/openharmony/os_openharmony.h"

#include <dlfcn.h>
#include <limits.h>
#include <native_buffer/native_buffer.h>
#include <native_image/native_image.h>
#include <native_window/external_window.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <clocale>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

struct WindowConsumer {
	OH_NativeImage *image = nullptr;
	OHNativeWindow *window = nullptr;
	std::atomic<bool> stopped{ false };
	std::atomic<unsigned> available{ 0 }, acquired{ 0 }, released{ 0 }, errors{ 0 };
	std::atomic<unsigned> initial_buffers{ 0 }, resized_buffers{ 0 }, invalid_buffers{ 0 };
	std::mutex notification_mutex;
	std::condition_variable condition;
	// NativeImage APIs are not thread-safe: resize, consume and destruction share
	// this mutex. The frame callback only updates the notification counter.
	std::mutex image_mutex;
	std::thread worker;
	bool listener_set = false;

	static void on_frame(void *p_context) {
		auto &self = *static_cast<WindowConsumer *>(p_context);
		self.available.fetch_add(1);
		self.condition.notify_one();
	}

	enum FenceResult { FENCE_SIGNALED,
		FENCE_FAILED,
		FENCE_INVALID };
	FenceResult wait_fence(int p_fence) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		for (;;) {
			const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
			if (remaining <= 0) {
				std::printf("CONSUMER fence_timeout fd=%d\n", p_fence);
				return FENCE_FAILED;
			}
			pollfd wait{ p_fence, POLLIN, 0 };
			const int result = poll(&wait, 1, static_cast<int>(remaining));
			if (result < 0 && errno == EINTR) {
				continue;
			}
			if (result <= 0 || (wait.revents & (POLLNVAL | POLLERR | POLLHUP)) || !(wait.revents & POLLIN)) {
				std::printf("CONSUMER fence_error fd=%d poll=%d revents=0x%x errno=%d\n", p_fence, result, wait.revents, result < 0 ? errno : 0);
				return (wait.revents & POLLNVAL) ? FENCE_INVALID : FENCE_FAILED;
			}
			return FENCE_SIGNALED;
		}
	}

	void consume() {
		unsigned seen = 0;
		for (;;) {
			{
				std::unique_lock<std::mutex> lock(notification_mutex);
				condition.wait_for(lock, std::chrono::milliseconds(20), [&] { return stopped.load() || available.load() != seen; });
				seen = available.load();
			}
			// Drain even after stop: the producer has already finished by then.
			for (;;) {
				std::lock_guard<std::mutex> image_lock(image_mutex);
				OHNativeWindowBuffer *buffer = nullptr;
				int fence = -1;
				const int status = OH_NativeImage_AcquireNativeWindowBuffer(image, &buffer, &fence);
				if (status == 40601000) { // NATIVE_ERROR_NO_BUFFER: the queue is empty.
					break;
				}
				if (status || !buffer) {
					++errors;
					std::printf("CONSUMER acquire_error=%d buffer=%d\n", status, buffer != nullptr);
					break;
				}
				++acquired;
				const int reference = OH_NativeWindow_NativeObjectReference(buffer);
				if (reference) {
					++errors;
					std::printf("CONSUMER reference_error=%d\n", reference);
				}
				const BufferHandle *handle = OH_NativeWindow_GetBufferHandleFromNative(buffer);
				if (handle && handle->width == 320 && handle->height == 240) {
					++initial_buffers;
				} else if (handle && handle->width == 640 && handle->height == 480) {
					++resized_buffers;
				} else {
					++invalid_buffers;
					++errors;
					std::printf("CONSUMER buffer_size_error width=%d height=%d\n", handle ? handle->width : -1, handle ? handle->height : -1);
				}
				int release_fence = -1;
				if (fence >= 0) {
					const FenceResult waited = wait_fence(fence);
					if (waited == FENCE_SIGNALED) {
						if (close(fence) != 0) {
							++errors;
							std::printf("CONSUMER fence_close_error errno=%d\n", errno);
						}
					} else {
						++errors;
						// A valid unsignaled fence is transferred to Release, which
						// closes it. An invalid descriptor is never treated as ready.
						if (waited != FENCE_INVALID) {
							release_fence = fence;
						}
					}
				} else if (fence != -1) {
					++errors;
					std::printf("CONSUMER invalid_negative_fence=%d\n", fence);
				}
				const int release = OH_NativeImage_ReleaseNativeWindowBuffer(image, buffer, release_fence);
				if (!reference) {
					const int unreference = OH_NativeWindow_NativeObjectUnreference(buffer);
					if (unreference) {
						++errors;
						std::printf("CONSUMER unreference_error=%d\n", unreference);
					}
				}
				if (release) {
					++errors;
					std::printf("CONSUMER release_error=%d\n", release);
				} else {
					++released;
				}
			}
			if (stopped.load()) {
				break;
			}
		}
	}

	bool begin() {
		image = OH_ConsumerSurface_Create();
		std::printf("CONSUMER create=%d\n", image != nullptr);
		if (!image) {
			return false;
		}
		const int size = OH_ConsumerSurface_SetDefaultSize(image, 320, 240);
		const int usage = OH_ConsumerSurface_SetDefaultUsage(image, NATIVEBUFFER_USAGE_HW_RENDER | NATIVEBUFFER_USAGE_HW_TEXTURE);
		window = OH_NativeImage_AcquireNativeWindow(image);
		uint64_t id = 0;
		const int id_status = OH_NativeImage_GetSurfaceId(image, &id);
		const int listener = OH_NativeImage_SetOnFrameAvailableListener(image, { this, on_frame });
		listener_set = listener == 0;
		std::printf("CONSUMER size=%d usage=%d window=%d surface_status=%d surface_id=%llu listener=%d\n", size, usage, window != nullptr, id_status, static_cast<unsigned long long>(id), listener);
		if (size || usage || !window || id_status || listener) {
			return false;
		}
		worker = std::thread([this] { consume(); });
		return true;
	}

	bool resize() {
		std::lock_guard<std::mutex> image_lock(image_mutex);
		const int consumer_size = OH_ConsumerSurface_SetDefaultSize(image, 640, 480);
		const int producer_size = OH_NativeWindow_NativeWindowHandleOpt(window, SET_BUFFER_GEOMETRY, 640, 480);
		std::printf("CONSUMER_RESIZE width=640 height=480 consumer=%d producer=%d\n", consumer_size, producer_size);
		return consumer_size == 0 && producer_size == 0;
	}

	void finish() {
		stopped = true;
		condition.notify_one();
		if (worker.joinable()) {
			worker.join();
		}
		std::lock_guard<std::mutex> image_lock(image_mutex);
		if (image) {
			if (listener_set) {
				const int unset = OH_NativeImage_UnsetOnFrameAvailableListener(image);
				if (unset) {
					++errors;
					std::printf("CONSUMER listener_unset_error=%d\n", unset);
				}
			}
			OH_NativeImage_Destroy(&image);
		}
		std::printf("CONSUMER_FINAL callbacks=%u acquired=%u released=%u errors=%u destroyed=%d\n", available.load(), acquired.load(), released.load(), errors.load(), image == nullptr);
		std::printf("CONSUMER_SIZES initial=%u resized=%u invalid=%u\n", initial_buffers.load(), resized_buffers.load(), invalid_buffers.load());
	}
};

int main(int argc, char **argv) {
	setvbuf(stdout, nullptr, _IOLBF, 0);
	setvbuf(stderr, nullptr, _IOLBF, 0);
	setlocale(LC_CTYPE, "");
	Dl_info library{};
	char actual_path[PATH_MAX] = {}, expected_path[PATH_MAX] = {};
	const char *expected = std::getenv("GODOT_RENDERING_WINDOW_LIBRARY");
	const bool located = dladdr(reinterpret_cast<void *>(&godot_host_start), &library) && library.dli_fname && realpath(library.dli_fname, actual_path);
	const bool library_matches = located && expected && realpath(expected, expected_path) && std::strcmp(actual_path, expected_path) == 0;
	std::printf("ENGINE_LIBRARY path=%s expected=%s match=%d\n", located ? actual_path : "unavailable", expected ? expected : "unset", library_matches);
	if (!library_matches) {
		return 26;
	}
	const char *resize_option = std::getenv("GODOT_RENDERING_WINDOW_RESIZE");
	const bool resize_requested = resize_option && std::strcmp(resize_option, "1") == 0;
	bool resized = false;
	WindowConsumer consumer;
	if (!consumer.begin()) {
		consumer.finish();
		return 20;
	}
	int result = 0;
	{
		// Use the same exported C++ bootstrap as the CLI. All renderers and actual
		// EGL/Vulkan calls execute in the selected signed library without shims.
		OS_OpenHarmony::EXEC_PATH = argv[0];
		OS_OpenHarmony os(false);
		os.set_native_window(consumer.window);
		os.set_window_id(-1);
		os.set_display_size(Size2i(320, 240));
		os.set_allowed_permissions("");
		std::printf("ENGINE_BOOTSTRAP cli-style native_window=true window_id=-1 headless=false resize_requested=%d\n", resize_requested);
		const Error setup = Main::setup(argv[0], argc - 1, argv + 1);
		std::printf("ENGINE_SETUP status=%d\n", static_cast<int>(setup));
		if (setup != OK) {
			result = 22;
		} else {
			const int start = Main::start();
			std::printf("ENGINE_START status=%d main_loop=%d\n", start, os.get_main_loop() != nullptr);
			if (start != 0 || !os.get_main_loop()) {
				result = 23;
			} else {
				os.main_loop_begin();
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
				bool quit = false;
				unsigned iterations = 0;
				for (;;) {
					if (resize_requested && !resized && iterations == 8) {
						DisplayServerOpenHarmony *display = DisplayServerOpenHarmony::get_singleton();
						if (!display || !consumer.resize()) {
							result = 27;
							break;
						}
						display->resize_window(640, 480);
						resized = true;
						std::printf("ENGINE_RESIZE iteration=%u width=640 height=480 public_display_resize=1\n", iterations);
					}
					quit = os.main_loop_iterate();
					++iterations;
					if (quit) {
						break;
					}
					if (std::chrono::steady_clock::now() > deadline) {
						result = 24;
						break;
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(8));
				}
				std::printf("ENGINE_LOOP quit=%d iterations=%u exit_code=%d\n", quit, iterations, os.get_exit_code());
				if (!result) {
					result = os.get_exit_code();
				}
				os.main_loop_end();
			}
			Main::cleanup();
		}
	}
	consumer.finish(); // The engine and rendering thread stop before the window.
	if (!result && (consumer.acquired.load() < 10 || consumer.acquired.load() != consumer.released.load() || consumer.errors.load() != 0)) {
		result = 25;
	}
	if (!result && resize_requested && (!resized || consumer.initial_buffers.load() == 0 || consumer.resized_buffers.load() < 5)) {
		result = 28;
	}
	std::printf("NATIVE_WINDOW_RUNNER exit=%d\n", result);
	return result;
}
