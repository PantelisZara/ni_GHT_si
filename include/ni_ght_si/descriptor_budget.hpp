#pragma once

#include <cstddef>
#include <optional>

namespace night {

// Capacity for managed temporary descriptors, separate from retained identities.
// Reservation itself opens no descriptors and never waits for capacity.
class DescriptorReservation {
public:
    static constexpr std::size_t maximum = 8;
    static std::optional<DescriptorReservation> try_acquire(std::size_t count) noexcept;

    ~DescriptorReservation();
    DescriptorReservation(const DescriptorReservation&) = delete;
    DescriptorReservation& operator=(const DescriptorReservation&) = delete;
    DescriptorReservation(DescriptorReservation&& other) noexcept;
    DescriptorReservation& operator=(DescriptorReservation&& other) noexcept;

    std::size_t count() const noexcept { return count_; }

private:
    explicit DescriptorReservation(std::size_t count) noexcept : count_(count) {}
    void reset() noexcept;
    std::size_t count_{};
};

} // namespace night
