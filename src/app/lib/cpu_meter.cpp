#include "cpu_meter.hpp"

#include <windows.h>

#include <intrin.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>

namespace wizz::app {

namespace {

/// One (QueryPerformanceCounter, TSC) reading taken close together.
struct ClockPair {
    std::int64_t qpc = 0;
    std::uint64_t tsc = 0;
};

[[nodiscard]] ClockPair read_clocks() noexcept
{
    // A pair counts only when nothing came between its readings (a preemption would put
    // a whole time slice between them); 2 µs of QPC is far below the 30 ms baseline.
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    const std::int64_t tolerance = std::max<std::int64_t>(1, frequency.QuadPart / 500'000);
    ClockPair best{};
    std::int64_t best_spread = -1;
    for (int attempt = 0; attempt < 1000; ++attempt) {
        LARGE_INTEGER before{};
        LARGE_INTEGER after{};
        QueryPerformanceCounter(&before);
        const std::uint64_t tsc = __rdtsc();
        QueryPerformanceCounter(&after);
        const std::int64_t spread = after.QuadPart - before.QuadPart;
        if (best_spread < 0 || spread < best_spread) {
            best = ClockPair{.qpc = before.QuadPart + spread / 2, .tsc = tsc};
            best_spread = spread;
        }
        if (spread <= tolerance) {
            break;
        }
    }
    return best;
}

[[nodiscard]] double measure_cycles_per_ms() noexcept
{
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    const ClockPair first = read_clocks();
    Sleep(30);
    const ClockPair second = read_clocks();
    const double ms = static_cast<double>(second.qpc - first.qpc) * 1000.0 / static_cast<double>(frequency.QuadPart);
    return static_cast<double>(second.tsc - first.tsc) / ms;
}

/// The CPUs of the process (its affinity mask, at first use).
[[nodiscard]] std::uint64_t process_cpus() noexcept
{
    DWORD_PTR process = 0;
    DWORD_PTR system = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system) != 0 && process != 0) {
        return static_cast<std::uint64_t>(process);
    }
    const DWORD present = std::clamp<DWORD>(GetActiveProcessorCount(0), 1, 64);
    return present == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << present) - 1;
}

[[nodiscard]] std::uint64_t mask() noexcept
{
    static const std::uint64_t cpus = process_cpus();
    return cpus;
}

[[nodiscard]] std::uint64_t idle_cycles() noexcept
{
    std::array<ULONG64, 64> idle{};  // one processor group at most: 64 CPUs
    ULONG length = static_cast<ULONG>(sizeof(idle));
    if (QueryIdleProcessorCycleTime(&length, idle.data()) == 0) {
        return 0;
    }
    const std::size_t count = std::min<std::size_t>(length / sizeof(ULONG64), idle.size());
    std::uint64_t sum = 0;
    for (std::size_t cpu = 0; cpu < count; ++cpu) {
        if ((mask() >> cpu) & 1U) {
            sum += idle[cpu];
        }
    }
    return sum;
}

[[nodiscard]] double share(std::uint64_t cycles, std::uint64_t wall) noexcept
{
    return wall > 0 ? static_cast<double>(cycles) / static_cast<double>(wall) : 0.0;
}

} // namespace

std::uint64_t CpuMeter::thread_cycles() noexcept
{
    ULONG64 cycles = 0;
    return QueryThreadCycleTime(GetCurrentThread(), &cycles) != 0 ? cycles : 0;
}

CpuMeter::Sample CpuMeter::sample()
{
    Sample now;
    now.thread = thread_cycles();
    ULONG64 cycles = 0;
    if (QueryProcessCycleTime(GetCurrentProcess(), &cycles) != 0) {
        now.process = cycles;
    }
    now.idle = idle_cycles();
    now.tsc = __rdtsc();
    return now;
}

CpuMeter::Interval CpuMeter::between(const Sample& from, const Sample& to)
{
    const auto delta = [](std::uint64_t a, std::uint64_t b) { return b > a ? b - a : std::uint64_t{0}; };
    const std::uint64_t wall = delta(from.tsc, to.tsc);
    const double per_ms = cycles_per_ms();
    Interval interval;
    interval.wall_ms = static_cast<double>(wall) / per_ms;
    interval.thread_ms = static_cast<double>(delta(from.thread, to.thread)) / per_ms;
    interval.idle_cpus = share(delta(from.idle, to.idle), wall);
    interval.own_cpus = share(delta(from.process, to.process), wall);
    interval.other_cpus = std::max(0.0, static_cast<double>(cpus()) - interval.idle_cpus - interval.own_cpus);
    return interval;
}

CpuMeter::Sample CpuMeter::skip(Sample start, const Sample& skip_from, const Sample& skip_to) noexcept
{
    const auto delta = [](std::uint64_t a, std::uint64_t b) { return b > a ? b - a : std::uint64_t{0}; };
    start.tsc += delta(skip_from.tsc, skip_to.tsc);
    start.thread += delta(skip_from.thread, skip_to.thread);
    start.idle += delta(skip_from.idle, skip_to.idle);
    start.process += delta(skip_from.process, skip_to.process);
    return start;
}

double CpuMeter::cycles_per_ms()
{
    static const double rate = measure_cycles_per_ms();
    return rate;
}

int CpuMeter::cpus()
{
    return std::max(1, std::popcount(mask()));
}

} // namespace wizz::app
