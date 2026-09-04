#pragma once

#include <esp_heap_caps.h>

#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

// Nothrow versions of std::make_unique. Return nullptr on allocation failure
// instead of calling abort() (the default when exceptions are disabled on ESP32).
//
// Single object:
//   auto obj = makeUniqueNoThrow<PNG>();
//   if (!obj) { LOG_ERR("TAG", "OOM"); return false; }
//
// Array:
//   auto buf = makeUniqueNoThrow<uint8_t[]>(size);
//   if (!buf) { LOG_ERR("TAG", "OOM"); return false; }
//   buf[0] = 0xFF;
//   someApi(buf.get(), size);
//
// PSRAM variants (heap_caps_malloc MALLOC_CAP_SPIRAM). The same nothrow /
// nullptr-on-OOM semantics; the deleter calls heap_caps_free (not free) so
// the buffer is correctly released to the PSRAM heap.
//
//   auto ctx = makeUniqueNoThrowPsram<BuildContext>();
//   auto buf = makeUniqueNoThrowPsram<uint8_t[]>(size);
//

template <typename T, typename... Args>
  requires(!std::is_array_v<T>)
std::unique_ptr<T> makeUniqueNoThrow(Args&&... args) {
  return std::unique_ptr<T>(new (std::nothrow) T(std::forward<Args>(args)...));
}

template <typename T>
  requires std::is_unbounded_array_v<T>
std::unique_ptr<T> makeUniqueNoThrow(size_t count) {
  using Elem = std::remove_extent_t<T>;
  return std::unique_ptr<T>(new (std::nothrow) Elem[count]());
}

// PSRAM-backed nothrow allocator. heap_caps_malloc(MALLOC_CAP_SPIRAM) places
// the allocation in PSRAM (or returns nullptr if PSRAM is full / unsupported).
// The custom deleter calls the destructor and heap_caps_free (matching cap),
// NOT free() — heap_caps_malloc memory must be released with heap_caps_free.
template <typename T, typename... Args>
  requires(!std::is_array_v<T>)
auto makeUniqueNoThrowPsram(Args&&... args) {
  using Deleter = void (*)(T*);
  void* p = heap_caps_malloc(sizeof(T), MALLOC_CAP_SPIRAM);
  if (!p) return std::unique_ptr<T, Deleter>(static_cast<T*>(nullptr), nullptr);
  T* obj = new (p) T(std::forward<Args>(args)...);
  return std::unique_ptr<T, Deleter>(obj, [](T* t) noexcept {
    if (t) {
      t->~T();
      heap_caps_free(t);
    }
  });
}

template <typename T>
  requires std::is_unbounded_array_v<T>
auto makeUniqueNoThrowPsram(size_t count) {
  using Elem = std::remove_extent_t<T>;
  using Deleter = void (*)(T*);
  void* p = heap_caps_malloc(count * sizeof(Elem), MALLOC_CAP_SPIRAM);
  if (!p) return std::unique_ptr<T, Deleter>(static_cast<T*>(nullptr), nullptr);
  Elem* arr = static_cast<Elem*>(p);
  return std::unique_ptr<T, Deleter>(arr, [](T* t) noexcept { heap_caps_free(t); });
}

// Helper struct to call a cleanup function on exit from any scope.
// Use with a lambda to avoid unnecessary allocations from std::function/std::bind:
// Example:
//   auto jpeg = makeUniqueNoThrow<JPEGDEC>();
//   ScopedCleanup cleanup{[&jpeg]{ jpeg->close(); }};
//
template <typename F>
struct [[nodiscard]] ScopedCleanup final {
  const F fn;
  explicit ScopedCleanup(F f) : fn{std::move(f)} {}
  ScopedCleanup(const ScopedCleanup&) = delete;
  ScopedCleanup& operator=(const ScopedCleanup&) = delete;
  ScopedCleanup(ScopedCleanup&&) = delete;
  ScopedCleanup& operator=(ScopedCleanup&&) = delete;
  ~ScopedCleanup() { fn(); }
};

template <typename F>
ScopedCleanup(F) -> ScopedCleanup<F>;
