#include "commands.h"
namespace gpu {
void CommandProcessor::execute_guest(uint32_t address, uint32_t bytes) {
    if ((address & 3) || (bytes & 3) || (uint64_t)address + bytes > (1ull << 32) || bytes > 64 * 1024 * 1024)
        throw std::runtime_error("invalid PM4 indirect buffer range");
    if (nesting >= 16) throw std::runtime_error("PM4 indirect buffer recursion limit");
    std::vector<uint32_t> words(bytes / 4);
    for (size_t i = 0; i < words.size(); i++) words[i] = read(address + i * 4);
    ++nesting;
    try { execute(words); }
    catch (...) { --nesting; throw; }
    --nesting;
}
void CommandProcessor::execute(std::span<const uint32_t> words) {
    while (!words.empty()) {
        uint32_t h = words[0]; words = words.subspan(1);
        uint32_t type = h >> 30;
        if (type == 2) continue;
        if (type != 0 && type != 3) throw std::runtime_error("invalid PM4 packet type");
        uint32_t count = ((h >> 16) & 0x3fff) + 1;
        if (count > words.size()) throw std::runtime_error("truncated PM4 packet");
        auto payload = words.first(count); words = words.subspan(count);
        if (type == 0) {
            uint32_t reg = h & 0xffff;
            if (reg + count > 0x10000) throw std::runtime_error("PM4 type-0 register overflow");
            std::copy(payload.begin(), payload.end(), registers.begin() + reg);
            continue;
        }
        uint8_t op = (h >> 8) & 0xff;
        uint32_t base;
        switch (op) {
        case 0x68: base = 0x2000; break;
        case 0x69: base = 0xa000; break;
        case 0x6a: base = 0xc000; break;
        case 0x6c: base = 0xf880; break;
        case 0x6d: base = 0xe000; break;
        case 0x6e: base = 0xf000; break;
        case 0x10: continue; // NOP
        case 0x32:
            if (count != 3 || payload[1] || payload[2] > UINT32_MAX / 4)
                throw std::runtime_error("invalid PM4 indirect buffer packet");
            execute_guest(payload[0], payload[2] * 4); continue;
        default: packet(op, payload); continue;
        }
        uint64_t reg = (uint64_t)base + payload[0];
        if (reg + count - 1 > 0x10000) throw std::runtime_error("PM4 register range overflow");
        std::copy(payload.begin() + 1, payload.end(), registers.begin() + reg);
    }
}
}
