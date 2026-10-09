#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <cstdlib>
#include <cstring>

namespace AgcDriver::DriverDetail {

bool Driver::traceBudget() {
    static std::atomic<int> lines{0};
    return lines.fetch_add(1) < 600;
}

void Driver::traceCaptureStability(const char* phase, std::uint64_t program, std::span<const ShaderRecompiler::MemoryRegion> regions) {
    static const std::uint64_t selected = [] {
        const auto* text = std::getenv("APS5_TRACE_CAPTURE_STATE");
        return text != nullptr ? std::strtoull(text, nullptr, 16) : 0ull;
    }();
    if (selected == 0 || selected != program) return;
    static std::atomic<unsigned> traces{0};
    if (traces.fetch_add(1, std::memory_order_relaxed) >= 128u) return;
    std::size_t differing = 0, unmapped = 0;
    std::vector<std::byte> live;
    for (const auto& region : regions) {
        live.resize(region.bytes.size());
        if (GuestMemory::CopyMapped(region.guestAddress, live) != GuestMemory::Compare::Equal) {
            ++unmapped;
            continue;
        }
        const auto difference = std::mismatch(region.bytes.begin(), region.bytes.end(), live.begin());
        if (difference.first == region.bytes.end()) continue;
        if (++differing > 8u) continue;
        const auto index = static_cast<std::size_t>(difference.first - region.bytes.begin());
        const auto wordOffset = index & ~std::size_t{3};
        const auto wordBytes = std::min(std::size_t{4}, region.bytes.size() - wordOffset);
        std::uint32_t expected = 0, actual = 0;
        std::memcpy(&expected, region.bytes.data() + wordOffset, wordBytes);
        std::memcpy(&actual, live.data() + wordOffset, wordBytes);
        std::fprintf(stderr, "[capture-state] %s program 0x%llx range=0x%llx+0x%zx mismatch=0x%zx word_offset=0x%zx word_bytes=%zu expected=0x%08x actual=0x%08x\n",
            phase, static_cast<unsigned long long>(program), static_cast<unsigned long long>(region.guestAddress), region.bytes.size(), index, wordOffset, wordBytes, expected, actual);
    }
    std::fprintf(stderr, "[capture-state] %s program 0x%llx regions=%zu differing=%zu unmapped=%zu\n", phase, static_cast<unsigned long long>(program), regions.size(), differing, unmapped);
}

void Driver::traceCapture(const char* what, std::uint64_t program, std::uint32_t queue, std::span<const ShaderRecompiler::MemoryRegion> regions, double waitedMs) {
    if (waitedMs < 0.05 || !traceBudget()) return;
    std::fprintf(stderr, "[capsync] %s q0x%x program 0x%llx waited %.1f ms over %zu regions\n", what, queue, static_cast<unsigned long long>(program), waitedMs, regions.size());
    for (const auto& region : regions) {
        const auto begin = region.guestAddress;
        const auto end = begin + region.bytes.size();
        const auto pageBegin = begin & ~static_cast<std::uint64_t>(4095);
        const auto pageEnd = (end + 4095) & ~static_cast<std::uint64_t>(4095);
        if (!newestWriter(pageBegin, pageEnd)) continue;
        std::fprintf(stderr, "[capsync]   region 0x%llx+0x%zx: writers of its page(s):%s; of its dwords:%s\n", static_cast<unsigned long long>(begin), region.bytes.size(), describeWriters(pageBegin, pageEnd).c_str(), describeWriters(begin, end).c_str());
    }
}

void Driver::reportValidation(ValidateCounters& counters) {
    const auto count = [](std::uint64_t value) { return static_cast<unsigned long long>(value); };
    std::size_t tracked = 0, eligible = 0;
    {
        std::lock_guard lock(writtenBuffersMutex);
        tracked = dwordEvidence.size();
        for (const auto& [address, evidence] : dwordEvidence) {
            if (evidence.streak >= writeEvidenceAfter()) ++eligible;
        }
    }
    AgcDriver::ProfilePrint_nid_no_patch( "[validate] dispatch-cache compares over pending GPU writes (10 s): %llu; missed without the sync %llu, skipped the sync %llu, synced %llu waiting %.0f ms (no dispatch writer %llu, foreign writer %llu, larger range %llu, label %llu, evidence short %llu, writer changed %llu, sampled %llu, image %llu, shadow %llu, off %llu); known-value reads %llu; verify: %llu would-skip compares synced anyway (%.0f ms), %llu mismatches, %llu misses a sync made hits; dwords observed over waits: %llu unchanged, %llu changed (regions not observed: %llu after no GPU wait, %llu not compared); %zu tracked, %zu with a streak\n", count(counters.pending), count(counters.unsyncedMisses), count(counters.skipped), count(counters.syncedNoWriter + counters.syncedForeign + counters.syncedLargeRange + counters.syncedLabel + counters.syncedEvidence + counters.syncedWriterChanged + counters.syncedSample + counters.syncedImage + counters.syncedShadow + counters.syncedOff), counters.syncedWaitMs, count(counters.syncedNoWriter), count(counters.syncedForeign), count(counters.syncedLargeRange), count(counters.syncedLabel), count(counters.syncedEvidence), count(counters.syncedWriterChanged), count(counters.syncedSample), count(counters.syncedImage), count(counters.syncedShadow), count(counters.syncedOff), count(counters.knownValue), count(counters.verified), counters.verifiedWaitMs, count(counters.mismatches), count(counters.verifiedMissesHit), count(observedUnchanged.exchange(0)), count(observedChanged.exchange(0)), count(observationsNoWait.exchange(0)), count(observationsNotReached.exchange(0)), tracked, eligible);
    counters = ValidateCounters{};
}

}
