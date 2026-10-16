// gflib3's memory helpers, as the game uses them.
//
// Allocations come from a memory resource (the layout of std::pmr::memory_resource). An object
// made by MakeUnique remembers the resource, size and alignment it came from, so it can give its
// memory back (UniquePtr, the layout of std::unique_ptr with that deleter). While an object is
// being constructed, its resource sits on a per-thread stack, so allocations the constructor
// makes come from the same resource.
#pragma once
#include <assert.h>
#include <new>
#include <nn/os.h>

#include "decomp.h"

namespace gfl::mem {

class MemoryResource {
public:
    virtual ~MemoryResource();
    void* Allocate(size_t size, size_t align) { return DoAllocate(size, align); }
    void Deallocate(void* p, size_t size, size_t align) { DoDeallocate(p, size, align); }

protected:
    virtual void* DoAllocate(size_t size, size_t align) = 0;
    virtual void DoDeallocate(void* p, size_t size, size_t align) = 0;
    virtual bool DoIsEqual(const MemoryResource& other) const = 0;
};

// The resources of objects under construction on this thread: a
// boost::container::static_vector<MemoryResource*, 16> in a TLS slot.
struct ResourceStack {
    MemoryResource* entries[16];
    u64 count;
};
DECOMP_SIZE(ResourceStack, 0x88);

extern nn::os::TlsSlot g_resourceStackSlot;

// boost::container's assertion failures (the game's strings: expression, file, function)
extern const char g_assertBadAllocExpr[], g_assertThrowExceptionFile[], g_assertThrowBadAllocFunc[];
extern const char g_assertNotEmptyExpr[], g_assertVectorFile[], g_assertPopBackFunc[];

[[noreturn]] inline void FailStackFull() {
    __assert_fail(g_assertBadAllocExpr, g_assertThrowExceptionFile, 0x36, g_assertThrowBadAllocFunc);
}

[[noreturn]] inline void FailStackEmpty() {
    __assert_fail(g_assertNotEmptyExpr, g_assertVectorFile, 0x7c7, g_assertPopBackFunc);
}

inline ResourceStack* GetResourceStack() {
    return reinterpret_cast<ResourceStack*>(nn::os::GetTlsValue(g_resourceStackSlot));
}

// The resource objects being constructed on this thread allocate from, or null
inline MemoryResource* GetCurrentResource() {
    ResourceStack* stack = GetResourceStack();
    if (stack == nullptr || stack->count == 0) {
        return nullptr;
    }
    return stack->entries[stack->count - 1];
}

// Makes a resource the current one for the lifetime of the scope
class ResourceScope {
public:
    explicit ResourceScope(MemoryResource* resource) : stack_(GetResourceStack()) {
        if (stack_->count >= 16) {
            FailStackFull();
        }
        stack_->entries[stack_->count] = resource;
        stack_->count++;
    }
    ~ResourceScope() {
        if (stack_->count == 0) {
            FailStackEmpty();
        }
        stack_->count--;
    }
    ResourceScope(const ResourceScope&) = delete;
    ResourceScope& operator=(const ResourceScope&) = delete;

private:
    ResourceStack* stack_;
};

// An owning pointer that frees through the resource the object came from
template <class T>
class UniquePtr {
public:
    UniquePtr() = default;
    UniquePtr(T* p, MemoryResource* resource, size_t size = sizeof(T), size_t align = alignof(T))
        : ptr_(p), resource_(resource), size_(size), align_(align) {}
    UniquePtr(UniquePtr&& other)
        : ptr_(other.ptr_), resource_(other.resource_), size_(other.size_), align_(other.align_) {
        other.ptr_ = nullptr;
    }
    // reset(other.release()), then take other's deleter (std::unique_ptr's order)
    UniquePtr& operator=(UniquePtr&& other) {
        T* old = ptr_;
        ptr_ = other.ptr_;
        other.ptr_ = nullptr;
        if (old != nullptr) {
            Destroy(old);
        }
        resource_ = other.resource_;
        size_ = other.size_;
        align_ = other.align_;
        return *this;
    }
    ~UniquePtr() { Reset(); }
    UniquePtr(const UniquePtr&) = delete;
    UniquePtr& operator=(const UniquePtr&) = delete;

    void Reset() {
        T* old = ptr_;
        ptr_ = nullptr;
        if (old != nullptr) {
            Destroy(old);
        }
    }
    T* Get() const { return ptr_; }
    T* operator->() const { return ptr_; }
    T& operator*() const { return *ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

private:
    void Destroy(T* p) {
        p->~T();
        resource_->Deallocate(p, size_, align_);
    }

    T* ptr_ = nullptr;
    MemoryResource* resource_ = nullptr;
    size_t size_ = 0;
    size_t align_ = 0;
};

// Allocates a T from the resource and constructs it with the resource current
template <class T, class... Args>
UniquePtr<T> MakeUnique(MemoryResource* resource, Args&&... args) {
    void* p = resource->Allocate(sizeof(T), alignof(T));
    ResourceScope scope(resource);
    return UniquePtr<T>(new (p) T(static_cast<Args&&>(args)...), resource);
}

}  // namespace gfl::mem
