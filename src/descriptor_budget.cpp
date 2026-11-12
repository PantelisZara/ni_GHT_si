#include "ni_ght_si/descriptor_budget.hpp"

#include <atomic>
#include <utility>

namespace night {
namespace {
constinit std::atomic<std::size_t> reserved{};
}

std::optional<DescriptorReservation> DescriptorReservation::try_acquire(std::size_t count) noexcept {
    if (!count || count > maximum) return {};
    auto current = reserved.load(std::memory_order_relaxed);
    while (count <= maximum - current) {
        if (reserved.compare_exchange_weak(current, current + count,
                                           std::memory_order_acquire, std::memory_order_relaxed))
            return DescriptorReservation{count};
    }
    return {};
}

DescriptorReservation::~DescriptorReservation() { reset(); }

DescriptorReservation::DescriptorReservation(DescriptorReservation&& other) noexcept
    : count_(std::exchange(other.count_, 0)) {}

DescriptorReservation& DescriptorReservation::operator=(DescriptorReservation&& other) noexcept {
    if (this != &other) {
        reset();
        count_ = std::exchange(other.count_, 0);
    }
    return *this;
}

void DescriptorReservation::reset() noexcept {
    const auto count = std::exchange(count_, 0);
    if (count) reserved.fetch_sub(count, std::memory_order_release);
}

} // namespace night
