// gflib3's object base classes, as the game's objects use them. An object remembers the memory
// resource it was made in and its allocation, and a reference-counted one destroys itself and gives
// its memory back when its last reference goes. gflib3 stays recompiled: the functions defined out
// of line here link to the originals (decomp/symbols.txt).
#pragma once
#include <new>

#include "decomp.h"
#include "gfl/mem.h"

namespace gfl {

class Object {
public:
    // The fields are zeroed, then the resource of whatever is being constructed on this thread is
    // recorded
    Object() { resource_ = mem::GetCurrentResource(); }
    virtual ~Object() {}
    // Objects are freed by whoever made them (RefObject::Destroy), never by delete
    static void operator delete(void*) {}

    mem::MemoryResource* resource_;   // where it was made
    void* block_ = nullptr;           // its allocation, recorded by whoever made it (Create)
    size_t size_ = 0;
    size_t align_ = 0;
    u64 unk28_ = 0;
    u32 unk30_ = 0;
    u64 unk38_ = 0;
    u32 unk40_ = 0;
};
DECOMP_SIZE(Object, 0x48);

// Reference counting, as an interface
class RefCounted {
public:
    virtual ~RefCounted() {}
    virtual void AddRef() = 0;
    virtual void Release() = 0;
};

// A reference-counted object. The count is atomic; Release destroys it at 0.
class RefObject : public Object, public RefCounted {
public:
    ~RefObject() override {}
    // Destroys it and gives its allocation back to the resource it was made in
    virtual void Destroy();
    void AddRef() override;
    void Release() override;

    u32 refCount_ = 0;
};
DECOMP_SIZE(RefObject, 0x58);

// A counted reference to a reference-counted object, counted through its RefCounted interface
template <class T>
class RefPtr {
public:
    RefPtr() = default;
    explicit RefPtr(T* p) : p_(p) {
        if (p_ != nullptr) {
            static_cast<RefCounted*>(p_)->AddRef();
        }
    }
    ~RefPtr() {
        if (p_ != nullptr) {
            static_cast<RefCounted*>(p_)->Release();
            p_ = nullptr;
        }
    }
    RefPtr(const RefPtr&) = delete;
    RefPtr& operator=(const RefPtr&) = delete;
    RefPtr(RefPtr&& other) : p_(other.p_) { other.p_ = nullptr; }
    // Lets go of its reference, then takes other's
    RefPtr& operator=(RefPtr&& other) {
        if (p_ != nullptr) {
            static_cast<RefCounted*>(p_)->Release();
            p_ = nullptr;
        }
        p_ = other.p_;
        other.p_ = nullptr;
        return *this;
    }
    // Takes a reference to p, then lets go of the old one
    RefPtr& operator=(T* p) {
        if (p != nullptr) {
            static_cast<RefCounted*>(p)->AddRef();
        }
        if (p_ != nullptr) {
            static_cast<RefCounted*>(p_)->Release();
            p_ = nullptr;
        }
        p_ = p;
        return *this;
    }

    void Reset() {
        T* old = p_;
        p_ = nullptr;
        if (old != nullptr) {
            static_cast<RefCounted*>(old)->Release();
        }
    }
    void Swap(RefPtr& other) {
        T* p = p_;
        p_ = other.p_;
        other.p_ = p;
    }
    T* Get() const { return p_; }
    T* operator->() const { return p_; }

private:
    T* p_ = nullptr;
};

// Makes a gflib3 object in a resource: `gfl::Create<T>()(resource, args...)`. The resource is the
// current one while the object is allocated and constructed, an allocation that fails dumps the
// resource, and the allocation is recorded in the object so Destroy can give it back. The game has
// a copy of this in each source file that uses it, called as a function object (the call passes an
// unused `this`), so it's never inlined.
template <class T>
struct Create {
    template <class... Args>
    [[gnu::noinline]] T* operator()(mem::MemoryResource* resource, Args&&... args) const {
        mem::ResourceScope scope(resource);
        void* block = resource->Allocate(sizeof(T), alignof(T));
        if (block == nullptr) {
            resource->Dump();
        }
        T* object = new (block) T(static_cast<Args&&>(args)...);
        object->resource_ = resource;
        object->block_ = block;
        object->size_ = sizeof(T);
        object->align_ = alignof(T);
        return object;
    }
};

}  // namespace gfl
