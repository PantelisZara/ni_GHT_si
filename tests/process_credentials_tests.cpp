#include "ni_ght_si/pinned_process.hpp"

#include <array>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

std::string status_text(const std::array<std::string, 8>& ids) {
    return "Uid: " + ids[0] + " " + ids[1] + " " + ids[2] + " " + ids[3] +
           "\nGid: " + ids[4] + " " + ids[5] + " " + ids[6] + " " + ids[7] + "\n";
}

void complete_credentials() {
    const night::ProcessCredentials expected{{10, 11, 12, 13}, {20, 21, 22, 23}};
    const auto parsed = night::parse_process_credentials(
        "Name:\tworker\nUid:\t10\t11\t12\t13\nGid:\t20\t21\t22\t23\nThreads:\t2\n");
    require(parsed && *parsed == expected, "all four UID and GID positions must be preserved");
    const auto reordered = night::parse_process_credentials(
        "Gid:  20\t21 22\t23 \r\nIgnored: Uid: 99 99 99 99\nUid:\t10 11 12 13");
    require(reordered && *reordered == expected, "field order, whitespace and unrelated fields must be supported");
    const auto boundaries = night::parse_process_credentials(
        "Uid: 0 4294967295 0000 0004294967295\nGid: 4294967295 0 4294967295 0\n");
    const night::ProcessCredentials expected_boundaries{
        {0, 4294967295ULL, 0, 4294967295ULL}, {4294967295ULL, 0, 4294967295ULL, 0}};
    require(boundaries && *boundaries == expected_boundaries,
            "zero, leading zeros and the full unsigned 32-bit boundary must parse exactly");
}

void invalid_records() {
    for (const auto text : {"", "Uid: 1 2 3 4\n", "Gid: 1 2 3 4\n",
            "Name: Uid: 1 2 3 4\nGid: 1 2 3 4\n",
            " Uid: 1 2 3 4\nGid: 1 2 3 4\n",
            "Uid: 1 2 3 4\n\tGid: 1 2 3 4\n",
            "uid: 1 2 3 4\nGid: 1 2 3 4\n",
            "Uid: 1 2 3\nGid: 1 2 3 4\n",
            "Uid: 1 2 3 4 5\nGid: 1 2 3 4\n",
            "Uid: 1 2 3 4\nGid: 1 2 3\n",
            "Uid: 1 2 3 4\nGid: 1 2 3 4 5\n",
            "Uid: 1 2 3 4\nGid: 1 2 3 4\nUid: 1 2 3 4\n",
            "Gid: 1 2 3 4\nUid: 1 2 3 4\nGid: 1 2 3 4\n",
            "Uid: 1 2 3 4\nGid: 1 2 3 4\nUid: broken\n",
            "Uid: broken\nUid: 1 2 3 4\nGid: 1 2 3 4\n"}) {
        require(!night::parse_process_credentials(text), "missing, malformed or duplicate credential record accepted");
    }
    std::string nul = "Uid: 1 2 3 4\nGid: 1 2 3 4\nIgnored: ";
    nul.push_back('\0');
    require(!night::parse_process_credentials(nul), "NUL anywhere in status must fail parsing");
}

void every_position() {
    const std::array<std::string, 8> original{"10", "11", "12", "13", "20", "21", "22", "23"};
    const auto baseline = night::parse_process_credentials(status_text(original));
    require(baseline.has_value(), "test baseline credentials did not parse");
    for (std::size_t position = 0; position < original.size(); ++position) {
        auto changed = original;
        changed[position] = "99";
        const auto parsed = night::parse_process_credentials(status_text(changed));
        require(parsed && *parsed != *baseline, "credential equality ignored a UID or GID position");
        for (const auto invalid : {"-1", "+1", "1x", "0x10", "1.0", "4294967296",
                "18446744073709551615", "18446744073709551616", "\xff"}) {
            changed[position] = invalid;
            require(!night::parse_process_credentials(status_text(changed)),
                    "nondecimal or out-of-range credential accepted in one of the eight positions");
        }
        changed[position] = std::string{"1\0", 2};
        require(!night::parse_process_credentials(status_text(changed)), "NUL inside credential accepted");
    }
}
} // namespace

int main() {
    try {
        complete_credentials();
        invalid_records();
        every_position();
        std::cout << "PASS strict UID/GID credential parsing and equality\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
