#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <functional>
#include <memory>
#include <new>
#include <cassert>
#include <cstring>
#include <unordered_map>
#include <mutex>
#include <type_traits>
#include <unistd.h>

namespace slab
{

    constexpr std::size_t SLAB_DEFAULT_ALIGN = 8;

    inline std::size_t page_size()
    {
#if defined(_WIN32)
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        return static_cast<std::size_t>(si.dwPageSize);
#else
        static std::size_t pg = 0;
        if (pg == 0)
        {
            long s = ::sysconf(_SC_PAGESIZE);
            pg = s > 0 ? static_cast<std::size_t>(s) : 4096u;
        }
        return pg;
#endif
    }

    struct BufCtl;
    struct Slab;

    struct BufCtl
    {
        void *buf{};
        BufCtl *next{};
        Slab *slab{};
    };

    struct Slab
    {
        Slab *next{this};
        Slab *prev{this};
        BufCtl *start{};
        void *free_list{};
        int bufcount{0};
        void *mem_base{};
        std::size_t mem_size{};
    };

    class Cache
    {
    public:
        using ctor_t = std::function<void(void *, std::size_t)>;
        using dtor_t = std::function<void(void *, std::size_t)>;

        Cache(std::string name, std::size_t obj_size, std::size_t align = 0,
              ctor_t ctor = nullptr, dtor_t dtor = nullptr, bool thread_safe = false)
            : name_(std::move(name)), size_(obj_size), ctor_(std::move(ctor)), dtor_(std::move(dtor)), thread_safe_(thread_safe)
        {
            const std::size_t pg = page_size();
            if (align == 0)
                align = SLAB_DEFAULT_ALIGN;
            effsize_ = align * ((obj_size + align - 1) / align);
            small_threshold_ = pg / 8;
            if (size_ <= small_threshold_)
            {
                slab_maxbuf_ = static_cast<int>((pg - sizeof(Slab)) / effsize_);
            }
            else
            {
                slab_maxbuf_ = 8;
            }
        }

        ~Cache()
        {
            destroy();
        }

        Cache(const Cache &) = delete;
        Cache &operator=(const Cache &) = delete;
        Cache(Cache &&) = delete;
        Cache &operator=(Cache &&) = delete;

        void *alloc()
        {
            Guard g(lock(), thread_safe_);

            if (!slabs_)
                grow_();
            if (slabs_->bufcount == slab_maxbuf_)
                grow_();

            void *buf = nullptr;
            if (size_ <= small_threshold_)
            {
                buf = slabs_->free_list;
                assert(buf && "internal error: free_list empty but expected free slot");
                slabs_->free_list = *reinterpret_cast<void **>(buf);
                ++slabs_->bufcount;
            }
            else
            {
                auto *b = static_cast<BufCtl *>(slabs_->free_list);
                assert(b && "internal error: free_list empty but expected free slot");
                slabs_->free_list = b->next;
                buf = b->buf;
                ++slabs_->bufcount;
            }

            if (ctor_)
                ctor_(buf, size_);

            if (slabs_->bufcount == slab_maxbuf_)
                move_to_back_(slabs_);
            return buf;
        }

        void free(void *buf)
        {
            if (!buf)
                return;
            Guard g(lock(), thread_safe_);

            const std::size_t pg = page_size();
            if (size_ <= small_threshold_)
            {
                auto addr = reinterpret_cast<std::uintptr_t>(buf);
                std::uintptr_t mask = ~(static_cast<std::uintptr_t>(pg) - 1);
                void *mem = reinterpret_cast<void *>(addr & mask);
                auto *slab = reinterpret_cast<Slab *>(reinterpret_cast<std::byte *>(mem) + pg - sizeof(Slab));

                if (dtor_)
                    dtor_(buf, size_);
                *reinterpret_cast<void **>(buf) = slab->free_list;
                slab->free_list = buf;
                --slab->bufcount;

                if (slab->bufcount == 0)
                {
                    remove_(slab);
                    ::free(mem);
                }
                else if (slab->bufcount == slab_maxbuf_ - 1)
                {
                    move_to_front_(slab);
                }
            }
            else
            {
                auto it = large_lookup_.find(buf);
                assert(it != large_lookup_.end() && "attempt to free pointer not from this Cache");
                BufCtl *b = it->second;
                Slab *slab = b->slab;

                if (dtor_)
                    dtor_(buf, size_);

                b->next = static_cast<BufCtl *>(slab->free_list);
                slab->free_list = b;
                --slab->bufcount;

                if (slab->bufcount == 0)
                {
                    BufCtl *cur = slab->start;
                    for (int i = 0; i < slab_maxbuf_; ++i)
                    {
                        large_lookup_.erase(cur[i].buf);
                    }
                    remove_(slab);
                    ::free(slab->mem_base);
                    delete[] slab->start;
                    delete slab;
                }
                else if (slab->bufcount == slab_maxbuf_ - 1)
                {
                    move_to_front_(slab);
                }
            }
        }

        void destroy()
        {
            Guard g(lock(), thread_safe_);
            while (slabs_)
            {
                Slab *slab = slabs_;
                remove_(slab);
                if (size_ <= small_threshold_)
                {
                    void *mem = reinterpret_cast<void *>(reinterpret_cast<std::byte *>(slab) - page_size() + sizeof(Slab));
                    (void)mem;
                    mem = reinterpret_cast<void *>(reinterpret_cast<std::byte *>(slab) - (page_size() - sizeof(Slab)));
                    ::free(mem);
                }
                else
                {
                    BufCtl *cur = slab->start;
                    for (int i = 0; i < slab_maxbuf_; ++i)
                    {
                        large_lookup_.erase(cur[i].buf);
                    }
                    ::free(slab->mem_base);
                    delete[] slab->start;
                    delete slab;
                }
            }
            slabs_ = slabs_back_ = nullptr;
        }

        const std::string &name() const noexcept { return name_; }
        std::size_t object_size() const noexcept { return size_; }
        std::size_t effective_size() const noexcept { return effsize_; }
        int slab_maxbuf() const noexcept { return slab_maxbuf_; }

    private:
        struct Guard
        {
            std::mutex *m{};
            bool active{};
            Guard(std::mutex *mm, bool act) : m(mm), active(act)
            {
                if (active && m)
                    m->lock();
            }
            ~Guard()
            {
                if (active && m)
                    m->unlock();
            }
        };
        std::mutex *lock() { return &mtx_; }

        void grow_()
        {
            const std::size_t pg = page_size();
            if (size_ <= small_threshold_)
            {
                void *mem = nullptr;
                if (posix_memalign(&mem, pg, pg) != 0 || mem == nullptr)
                {
                    throw std::bad_alloc();
                }
                auto *slab = reinterpret_cast<Slab *>(reinterpret_cast<std::byte *>(mem) + pg - sizeof(Slab));
                new (slab) Slab();
                slab->free_list = mem;
                slab->mem_base = mem;
                slab->mem_size = pg;

                std::byte *p = static_cast<std::byte *>(mem);
                std::byte *last = static_cast<std::byte *>(mem) + effsize_ * (slab_maxbuf_ - 1);
                for (; p < last; p += effsize_)
                {
                    *reinterpret_cast<void **>(p) = p + effsize_;
                }
                *reinterpret_cast<void **>(last) = nullptr;

                move_to_front_(slab);
                assert(slabs_ == slab);
            }
            else
            {
                std::size_t need = effsize_ * static_cast<std::size_t>(slab_maxbuf_);
                std::size_t pages = (need + pg - 1) / pg;
                std::size_t memsz = pages * pg;
                void *mem = nullptr;
                if (posix_memalign(&mem, pg, memsz) != 0 || mem == nullptr)
                {
                    throw std::bad_alloc();
                }

                auto *slab = new Slab();
                slab->mem_base = mem;
                slab->mem_size = memsz;

                BufCtl *arr = new BufCtl[slab_maxbuf_];
                slab->start = arr;
                slab->free_list = &arr[0];
                arr[0].buf = mem;
                arr[0].slab = slab;
                arr[0].next = nullptr;
                large_lookup_.emplace(arr[0].buf, &arr[0]);

                for (int i = 1; i < slab_maxbuf_; ++i)
                {
                    arr[i].next = static_cast<BufCtl *>(slab->free_list);
                    arr[i].buf = static_cast<void *>(static_cast<std::byte *>(mem) + i * effsize_);
                    arr[i].slab = slab;
                    slab->free_list = &arr[i];
                    large_lookup_.emplace(arr[i].buf, &arr[i]);
                }
                move_to_front_(slab);
            }
        }

        void remove_(Slab *slab)
        {
            slab->next->prev = slab->prev;
            slab->prev->next = slab->next;

            if (slabs_ == slab)
            {
                if (slab->prev == slab)
                    slabs_ = nullptr;
                else
                    slabs_ = slab->prev;
            }
            if (slabs_back_ == slab)
            {
                if (slab->next == slab)
                    slabs_back_ = nullptr;
                else
                    slabs_back_ = slab->next;
            }
        }

        void move_to_front_(Slab *slab)
        {
            if (slabs_ == slab)
                return;
            if (slab->next || slab->prev)
            {
                remove_(slab);
            }
            if (!slabs_)
            {
                slab->prev = slab->next = slab;
                slabs_back_ = slab;
            }
            else
            {
                slab->prev = slabs_;
                slabs_->next = slab;
                slab->next = slabs_back_;
                slabs_back_->prev = slab;
            }
            slabs_ = slab;
        }

        void move_to_back_(Slab *slab)
        {
            if (slabs_back_ == slab)
                return;
            remove_(slab);
            if (!slabs_)
            {
                slab->prev = slab->next = slab;
                slabs_ = slab;
            }
            else
            {
                slab->prev = slabs_;
                slabs_->next = slab;
                slab->next = slabs_back_;
                slabs_back_->prev = slab;
            }
            slabs_back_ = slab;
        }

    private:
        std::string name_;
        std::size_t size_{0};
        std::size_t effsize_{0};
        std::size_t small_threshold_{0};
        int slab_maxbuf_{0};
        ctor_t ctor_;
        dtor_t dtor_;
        Slab *slabs_{nullptr};
        Slab *slabs_back_{nullptr};

        std::unordered_map<void *, BufCtl *> large_lookup_;
        std::mutex mtx_;
        bool thread_safe_{false};
    };

    template <class T>
    class TypedCache
    {
        static_assert(!std::is_void_v<T>, "T must be a complete type");

    public:
        explicit TypedCache(std::string name, std::size_t align = 0, bool thread_safe = false)
            : cache_(std::move(name), sizeof(T), align, [](void *p, std::size_t)
                     { new (p) T(); }, [](void *p, std::size_t)
                     { reinterpret_cast<T *>(p)->~T(); }, thread_safe) {}

        T *alloc() { return static_cast<T *>(cache_.alloc()); }
        void free(T *p) { cache_.free(static_cast<void *>(p)); }
        std::size_t effective_size() const { return cache_.effective_size(); }
        int slab_maxbuf() const { return cache_.slab_maxbuf(); }

    private:
        Cache cache_;
    };

} // namespace slab
