#include "SceTypes.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <limits>
#include <thread>

extern "C" {
KernelCpumask APS5_VABI sceKernelGetAvailableCpumask();
int APS5_VABI scePthreadCreate(Pthread* thread, const PthreadAttr* attr, PthreadEntry entry, void* arg, const char* name);
int APS5_VABI scePthreadJoin(Pthread thread, void** retval);
int APS5_VABI scePthreadGetthreadid();
Pthread APS5_VABI scePthreadSelf();
int APS5_VABI scePthreadGetaffinity(Pthread thread, KernelCpumask* mask);
int APS5_VABI cpuset_getaffinity_nid_postfix(int level, int which, std::int64_t id, std::size_t size, void* mask);
int APS5_VABI cpuset_setaffinity_nid_postfix(int level, int which, std::int64_t id, std::size_t size, const void* mask);
int* APS5_VABI __error_nid_postfix();
}

static void Require(bool value) {
    if (!value) std::abort();
}

struct WorkerState {
    std::promise<int> id;
    std::promise<void> release;
    KernelCpumask finalMask = 0;
};

static void* APS5_VABI Worker(void* arg) {
    auto* state = static_cast<WorkerState*>(arg);
    state->id.set_value(scePthreadGetthreadid());
    state->release.get_future().get();
    Require(scePthreadGetaffinity(scePthreadSelf(), &state->finalMask) == 0);
    return nullptr;
}

static KernelCpumask ReadMask(std::int64_t id) {
    std::uint64_t set[5] = {~0ull, ~0ull, ~0ull, ~0ull, ~0ull};
    Require(cpuset_getaffinity_nid_postfix(3, 1, id, 32, set) == 0);
    Require(set[1] == 0 && set[2] == 0 && set[3] == 0 && set[4] == ~0ull);
    return set[0];
}

static void RequireMissing(std::int64_t id, KernelCpumask available) {
    KernelCpumask untouched = ~0ull;
    Require(cpuset_getaffinity_nid_postfix(3, 1, id, sizeof(untouched), &untouched) == -1);
    Require(*__error_nid_postfix() == 3 && untouched == ~0ull);
    Require(cpuset_setaffinity_nid_postfix(3, 1, id, sizeof(available), &available) == -1);
    Require(*__error_nid_postfix() == 3);
}

int main() {
    const auto available = sceKernelGetAvailableCpumask();
    const auto first = available & (~available + 1);
    const int self = scePthreadGetthreadid();
    const auto original = ReadMask(-1);
    Require(ReadMask(self) == original);
    Require(cpuset_setaffinity_nid_postfix(3, 1, self, sizeof(first), &first) == 0);
    Require(ReadMask(-1) == first);
    Require(cpuset_setaffinity_nid_postfix(3, 1, self, sizeof(original), &original) == 0);

    WorkerState state;
    Pthread worker = nullptr;
    Require(scePthreadCreate(&worker, nullptr, Worker, &state, nullptr) == 0);
    const int id = state.id.get_future().get();
    Require(id != self && ReadMask(id) == original);
    Require(cpuset_setaffinity_nid_postfix(3, 1, id, sizeof(first), &first) == 0);
    Require(ReadMask(id) == first && ReadMask(self) == original);
    KernelCpumask byHandle = 0;
    Require(scePthreadGetaffinity(worker, &byHandle) == 0 && byHandle == first);
    std::uint64_t set[4] = {first | (1ull << 40), 0, 0, 0};
    Require(cpuset_setaffinity_nid_postfix(3, 1, id, sizeof(set), set) == 0);
    Require(ReadMask(id) == first);
    set[3] = 1;
    Require(cpuset_setaffinity_nid_postfix(3, 1, id, sizeof(set), set) == -1 && *__error_nid_postfix() == 22);
    Require(ReadMask(id) == first);
    Require(cpuset_getaffinity_nid_postfix(3, 1, id, sizeof(first), nullptr) == -1 && *__error_nid_postfix() == 14);
    Require(cpuset_setaffinity_nid_postfix(3, 1, id, sizeof(first), nullptr) == -1 && *__error_nid_postfix() == 14);
    for (const std::int64_t missing : {std::int64_t{0}, std::int64_t{-2}, std::numeric_limits<std::int64_t>::max()}) RequireMissing(missing, available);

    state.release.set_value();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    KernelCpumask finishedMask = 0;
    while (cpuset_getaffinity_nid_postfix(3, 1, id, sizeof(finishedMask), &finishedMask) == 0) {
        Require(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
    Require(*__error_nid_postfix() == 3);
    RequireMissing(id, available);
    Require(scePthreadJoin(worker, nullptr) == 0 && state.finalMask == first);
    RequireMissing(id, available);

    for (int iteration = 0; iteration < 32; ++iteration) {
        WorkerState racing;
        Require(scePthreadCreate(&worker, nullptr, Worker, &racing, nullptr) == 0);
        const int racingId = racing.id.get_future().get();
        std::atomic<bool> started{false}, stop{false};
        std::thread observer([&] {
            Require(ReadMask(racingId) == original);
            started.store(true, std::memory_order_release);
            while (!stop.load(std::memory_order_acquire)) {
                KernelCpumask mask = 0;
                const int result = cpuset_getaffinity_nid_postfix(3, 1, racingId, sizeof(mask), &mask);
                Require((result == 0 && mask == original) || (result == -1 && *__error_nid_postfix() == 3));
                const int changed = cpuset_setaffinity_nid_postfix(3, 1, racingId, sizeof(original), &original);
                Require(changed == 0 || (changed == -1 && *__error_nid_postfix() == 3));
            }
        });
        while (!started.load(std::memory_order_acquire)) std::this_thread::yield();
        racing.release.set_value();
        Require(scePthreadJoin(worker, nullptr) == 0);
        stop.store(true, std::memory_order_release);
        observer.join();
        RequireMissing(racingId, available);
    }
}
