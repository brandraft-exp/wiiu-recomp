#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <stdexcept>
#include <vector>

namespace gpu {
/* PM4 type-3 register and indirect-buffer decoding. Offsets and opcode values
 * follow Cemu LattePM4.h and LatteCommandProcessor.cpp. Unsupported packets go to a backend
 * callback; they are never silently discarded by this decoder. */
class CommandProcessor {
public:
    std::array<uint32_t, 0x10009> registers{};
    using Packet = std::function<void(uint8_t, std::span<const uint32_t>)>;
    using ReadWord = std::function<uint32_t(uint32_t)>;
    CommandProcessor(ReadWord reader, Packet backend) : read(std::move(reader)), packet(std::move(backend)) {}
    void execute(std::span<const uint32_t> words);
    void execute_guest(uint32_t address, uint32_t bytes);
private:
    ReadWord read;
    Packet packet;
    unsigned nesting = 0;
};
constexpr uint32_t header(uint8_t op, uint32_t words) { return 0xc0000000u | (words - 1) << 16 | uint32_t(op) << 8; }
}
