#pragma once

#include <new>
#include <utility>

namespace omega {

// Wraps a process-lifetime singleton whose destructor must never run during __cxa_finalize,
// so background/CLR ThreadPool threads cannot hit a destroyed mutex or container at exit.
template <typename T>
class NoDestroy {
public:
    template <typename... Args>
    explicit NoDestroy(Args&&... args) {
        ::new (static_cast<void*>(storage_)) T(std::forward<Args>(args)...);
    }
    ~NoDestroy() = default;
    NoDestroy(const NoDestroy&) = delete;
    NoDestroy& operator=(const NoDestroy&) = delete;

    T* get() noexcept { return std::launder(reinterpret_cast<T*>(storage_)); }
    const T* get() const noexcept { return std::launder(reinterpret_cast<const T*>(storage_)); }
    T& operator*() noexcept { return *get(); }
    const T& operator*() const noexcept { return *get(); }
    T* operator->() noexcept { return get(); }
    const T* operator->() const noexcept { return get(); }

private:
    alignas(T) unsigned char storage_[sizeof(T)];
};

}  // namespace omega
