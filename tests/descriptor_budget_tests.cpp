#include "ni_ght_si/descriptor_budget.hpp"

#include <array>
#include <atomic>
#include <barrier>
#include <iostream>
#include <latch>
#include <limits>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using night::DescriptorReservation;
static_assert(!std::is_copy_constructible_v<DescriptorReservation>);
static_assert(!std::is_copy_assignable_v<DescriptorReservation>);
static_assert(std::is_nothrow_move_constructible_v<DescriptorReservation>);
static_assert(std::is_nothrow_move_assignable_v<DescriptorReservation>);
static_assert(std::is_nothrow_destructible_v<DescriptorReservation>);
static_assert(noexcept(DescriptorReservation::try_acquire(1)));

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void full_capacity_restored() {
    const auto reservation = DescriptorReservation::try_acquire(DescriptorReservation::maximum);
    require(reservation && reservation->count() == DescriptorReservation::maximum,
            "released reservations did not restore the whole pool");
}

void weighted_capacity() {
    require(!DescriptorReservation::try_acquire(0), "zero descriptor request accepted");
    require(!DescriptorReservation::try_acquire(DescriptorReservation::maximum + 1), "oversized request accepted");
    require(!DescriptorReservation::try_acquire(std::numeric_limits<std::size_t>::max()),
            "overflow-sized request accepted");
    {
        auto first = DescriptorReservation::try_acquire(3);
        auto middle = DescriptorReservation::try_acquire(2);
        auto last = DescriptorReservation::try_acquire(3);
        require(first && first->count() == 3 && middle && middle->count() == 2 && last && last->count() == 3,
                "weighted requests did not fill the eight-unit pool");
        require(!DescriptorReservation::try_acquire(1), "full pool admitted another reservation");
        middle.reset();
        require(!DescriptorReservation::try_acquire(3), "failed weighted request partially consumed capacity");
        auto recovered = DescriptorReservation::try_acquire(2);
        require(recovered && recovered->count() == 2 && !DescriptorReservation::try_acquire(1),
                "exactly released capacity could not be reacquired");
    }
    full_capacity_restored();
    for (std::size_t weight = 1; weight < DescriptorReservation::maximum; ++weight) {
        auto first = DescriptorReservation::try_acquire(weight);
        auto complement = DescriptorReservation::try_acquire(DescriptorReservation::maximum - weight);
        require(first && first->count() == weight && complement &&
                complement->count() == DescriptorReservation::maximum - weight &&
                !DescriptorReservation::try_acquire(1), "complementary weights did not preserve the exact cap");
    }
    full_capacity_restored();
}

void moves_and_unwinding() {
    {
        auto source = DescriptorReservation::try_acquire(3);
        require(source.has_value(), "move test could not reserve capacity");
        DescriptorReservation moved{std::move(*source)};
        require(source->count() == 0 && moved.count() == 3 && !DescriptorReservation::try_acquire(6),
                "move construction duplicated or lost weighted ownership");
        auto destination = DescriptorReservation::try_acquire(2);
        require(destination.has_value(), "move assignment destination could not reserve capacity");
        *destination = std::move(moved);
        require(moved.count() == 0 && destination->count() == 3,
                "move assignment did not clear the source and transfer its count");
        auto& same = *destination;
        same = std::move(*destination);
        require(same.count() == 3, "self move released its owned reservation");
        auto remaining = DescriptorReservation::try_acquire(5);
        require(remaining && remaining->count() == 5 && !DescriptorReservation::try_acquire(1),
                "move assignment failed to release the destination's former capacity");
        source.reset();
        require(!DescriptorReservation::try_acquire(1), "destroying a moved-from owner released capacity twice");
    }
    full_capacity_restored();
    struct Unwind {};
    bool caught{};
    try {
        auto reservation = DescriptorReservation::try_acquire(DescriptorReservation::maximum);
        require(reservation.has_value(), "exception test could not reserve the whole pool");
        throw Unwind{};
    } catch (const Unwind&) { caught = true; }
    require(caught, "exception fixture did not unwind its reservation");
    full_capacity_restored();
}

void concurrent_weighted_admission() {
    constexpr std::size_t participants = 12;
    std::array<std::size_t, participants> held{};
    std::latch start{1};
    std::atomic_bool cancelled{};
    std::barrier acquired{static_cast<std::ptrdiff_t>(participants + 1)};
    std::barrier release{static_cast<std::ptrdiff_t>(participants + 1)};
    std::vector<std::jthread> workers;
    workers.reserve(participants);
    try {
        for (std::size_t i = 0; i < participants; ++i) {
            workers.emplace_back([&, i] {
                start.wait();
                if (cancelled.load()) return;
                auto reservation = DescriptorReservation::try_acquire(2);
                held[i] = reservation ? reservation->count() : 0;
                acquired.arrive_and_wait();
                release.arrive_and_wait();
            });
        }
    } catch (...) {
        cancelled.store(true);
        start.count_down();
        throw;
    }
    start.count_down();
    acquired.arrive_and_wait();
    std::size_t successes{}, total{};
    for (const auto count : held) { if (count) ++successes; total += count; }
    const bool full_rejected = !DescriptorReservation::try_acquire(1);
    // Release and join before assertions so a failed assertion cannot leave
    // successful workers holding their reservation at a barrier indefinitely.
    release.arrive_and_wait();
    workers.clear();
    require(successes == 4 && total == DescriptorReservation::maximum && full_rejected,
            "concurrent two-unit requests did not admit exactly four owners within eight units");
    full_capacity_restored();
}
} // namespace

int main() {
    try {
        weighted_capacity();
        moves_and_unwinding();
        concurrent_weighted_admission();
        std::cout << "PASS weighted temporary descriptor cap, move/unwind ownership and concurrent admission\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
