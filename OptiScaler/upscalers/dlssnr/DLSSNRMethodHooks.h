#pragma once
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>
#include <windows.h>
#include <tlhelp32.h>
#include <detours/detours.h>

// A command-list method can have both runtime and driver entry points. Each
// entry needs its own trampoline; replacing a single global original with the
// latest vtable address routes older lists to the wrong implementation.
namespace DLSSNRMethodHooks
{
inline std::mutex installationMutex;

// Runtime discovery happens while other game threads may be recording. Enlist
// their instruction pointers in the Detours transaction as well as our own.
// Allocate the handle list before suspending any threads.
struct Threads
{
    std::vector<HANDLE> handles;
    LONG status = NO_ERROR;
    Threads()
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) { status = GetLastError(); return; }
        THREADENTRY32 thread {sizeof(THREADENTRY32)};
        if (!Thread32First(snapshot, &thread)) status = GetLastError();
        else do
        {
            if (thread.th32OwnerProcessID != GetCurrentProcessId() ||
                thread.th32ThreadID == GetCurrentThreadId()) continue;
            HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                                       THREAD_QUERY_INFORMATION | SYNCHRONIZE, FALSE, thread.th32ThreadID);
            if (handle)
            {
                if (GetProcessIdOfThread(handle) == GetCurrentProcessId()) handles.push_back(handle);
                else CloseHandle(handle);
            }
            else if (const auto error = GetLastError(); error != ERROR_INVALID_PARAMETER)
            { status = error; break; }
        } while (Thread32Next(snapshot, &thread));
        CloseHandle(snapshot);
    }
    ~Threads() { for (auto handle : handles) CloseHandle(handle); }
    LONG Enlist()
    {
        for (auto handle : handles)
        {
            if (WaitForSingleObject(handle, 0) == WAIT_OBJECT_0) continue;
            const auto result = DetourUpdateThread(handle);
            if (result != NO_ERROR) return result;
        }
        return DetourUpdateThread(GetCurrentThread());
    }
};

template<unsigned Method, typename Function, bool TrackingOnly = false, bool (*ShouldObserve)() = nullptr>
struct MethodHook;
template<unsigned Method, bool TrackingOnly, bool (*ShouldObserve)(), typename Return, typename Object, typename... Args>
struct MethodHook<Method, Return(STDMETHODCALLTYPE*)(Object*, Args...), TrackingOnly, ShouldObserve>
{
    using Function = Return(STDMETHODCALLTYPE*)(Object*, Args...);
    static constexpr size_t Capacity = 8;
    struct Entry
    {
        std::atomic<void*> target {nullptr};
        Function original = nullptr;
        Function callback = nullptr;
        void* detourCode = nullptr;
        std::array<unsigned char, 16> detourBytes {};
        void* patchCode = nullptr;
        std::array<unsigned char, 16> patchBytes {};
    };
    inline static std::array<Entry, Capacity> entries {};
    inline static thread_local Entry* current = nullptr;
    inline static thread_local Object* currentObject = nullptr;

    template<size_t Index> static Return STDMETHODCALLTYPE Invoke(Object* object, Args... args)
    {
        auto& entry = entries[Index];
        // Inactive observers need neither a callback nor the per-method TLS
        // scope. Keep forwarding through the original chain (including other
        // consumers' hooks), so observation can resume without reinstalling.
        if constexpr (ShouldObserve != nullptr)
            if (!ShouldObserve()) return entry.original(object, args...);
        // A driver implementation can forward to the runtime entry. Process
        // that operation once, while still allowing calls on another list.
        if (current && currentObject == object)
        {
            return entry.original(object, args...);
        }
        struct Scope
        {
            Entry* previous = current;
            Object* previousObject = currentObject;
            ~Scope() { current = previous; currentObject = previousObject; }
        } scope;
        current = &entry;
        currentObject = object;
        return entry.callback(object, args...);
    }
    template<size_t... I> static constexpr auto Thunks(std::index_sequence<I...>)
    {
        return std::array<Function, Capacity> {&Invoke<I>...};
    }
    inline static constexpr auto thunks = Thunks(std::make_index_sequence<Capacity>{});

    static Return Forward(Object* object, Args... args)
    {
        return current->original(object, args...);
    }
    static bool Contains(void* target)
    {
        for (auto& entry : entries)
            if (entry.target.load(std::memory_order_acquire) == target) return true;
        return false;
    }
    // Returns only after commit; a failed transaction never publishes a target.
    static LONG Install(void* target, Function callback, bool* added = nullptr)
    {
        if (added) *added = false;
        if (!target || !callback) return ERROR_INVALID_PARAMETER;
        if (Contains(target)) return NO_ERROR;
        std::lock_guard lock(installationMutex);
        if (Contains(target)) return NO_ERROR;
        for (size_t i = 0; i < Capacity; ++i)
        {
            auto& entry = entries[i];
            if (entry.target.load()) continue;
            Threads threads;
            if (threads.status != NO_ERROR) return threads.status;
            LONG result = DetourTransactionBegin();
            if (result != NO_ERROR) return result;
            entry.original = reinterpret_cast<Function>(target);
            entry.callback = callback;
            entry.detourCode = DetourCodeFromPointer(reinterpret_cast<PVOID>(thunks[i]), nullptr);
            std::memcpy(entry.detourBytes.data(), entry.detourCode, entry.detourBytes.size());
            result = threads.Enlist();
            if (result == NO_ERROR)
                result = DetourAttachEx(reinterpret_cast<PVOID*>(&entry.original), reinterpret_cast<PVOID>(thunks[i]),
                                        nullptr, &entry.patchCode, nullptr);
            if (result == NO_ERROR) result = DetourTransactionCommit();
            else DetourTransactionAbort();
            if (result != NO_ERROR)
            {
                entry.original = nullptr;
                entry.callback = nullptr;
                return result;
            }
            std::memcpy(entry.patchBytes.data(), entry.patchCode, entry.patchBytes.size());
            entry.target.store(target, std::memory_order_release);
            if (added) *added = true;
            return NO_ERROR;
        }
        return ERROR_TOO_MANY_CMDS;
    }
    // Call during the same quiescent teardown used by the other D3D12 hooks.
    // On failure keep the live trampoline and metadata intact.
    static LONG Remove()
    {
        std::lock_guard lock(installationMutex);
        bool any = false;
        for (auto& entry : entries)
        {
            if (!entry.target.load()) continue;
            any = true;
            // Detours can chain a later interceptor into our thunk. Never free
            // the lower trampoline while that interceptor still refers to it.
            // Keep it alive and retry after the outer hook has been removed.
            if (std::memcmp(entry.detourBytes.data(), entry.detourCode, entry.detourBytes.size()) != 0 ||
                std::memcmp(entry.patchBytes.data(), entry.patchCode, entry.patchBytes.size()) != 0)
                return ERROR_BUSY;
        }
        if (!any) return NO_ERROR;
        Threads threads;
        if (threads.status != NO_ERROR) return threads.status;
        LONG result = DetourTransactionBegin();
        if (result != NO_ERROR) return result;
        result = threads.Enlist();
        for (size_t i = 0; result == NO_ERROR && i < Capacity; ++i)
            if (entries[i].target.load())
                result = DetourDetach(reinterpret_cast<PVOID*>(&entries[i].original), reinterpret_cast<PVOID>(thunks[i]));
        if (result == NO_ERROR) result = DetourTransactionCommit();
        else DetourTransactionAbort();
        if (result == NO_ERROR)
            for (auto& entry : entries)
            {
                entry.target.store(nullptr, std::memory_order_release);
                entry.original = nullptr;
                entry.callback = nullptr;
            }
        return result;
    }
};
}
