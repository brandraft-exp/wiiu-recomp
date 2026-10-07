#include "gx2/commands.h"
#include <cassert>
#include <map>
using gpu::header;
int main() {
    std::map<uint32_t,uint32_t> memory;
    unsigned draws = 0;
    gpu::CommandProcessor cp([&](uint32_t a) { return memory.at(a); },
        [&](uint8_t op, std::span<const uint32_t> words) {
            assert(op == 0x2d && words.size() == 2 && words[0] == 4);
            ++draws;
        });
    uint32_t commands[] = {header(0x69,3),0x100,42,43, 0x80000000, header(0x2d,2),4,2};
    for (unsigned i=0;i<8;i++) memory[0x1000+i*4]=commands[i];
    uint32_t indirect[] = {header(0x32,3),0x1000,0,8};
    cp.execute(indirect);
    assert(draws == 1 && cp.registers[0xa100] == 42 && cp.registers[0xa101] == 43);
    uint32_t overflow[] = {header(0x69,2),0xffffffff,7};
    uint32_t truncated[] = {header(0x69,3),0x100,42};
    auto rejects = [&](std::span<const uint32_t> words) {
        try { cp.execute(words); } catch (const std::runtime_error&) { return true; }
        return false;
    };
    assert(rejects(overflow)); assert(rejects(truncated));
    for (unsigned i=0;i<4;i++) memory[0x1000+i*4]=indirect[i];
    memory[0x100c] = 4;
    assert(rejects(indirect));
    // An exception must not poison the recursion counter for subsequent work.
    cp.execute(commands); assert(draws == 2);
}
