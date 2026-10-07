/* Standalone, growing string buffer implementing the Cemu emitter interface. */
#pragma once
class StringBuf {
    std::string value;
public:
    explicit StringBuf(uint32 reserve) { value.reserve(reserve); }
    template<typename F, typename... Args> void addFmt(const F& f, Args&&... args) {
        value += fmt::vformat(f, fmt::make_format_args(args...));
    }
    void add(std::string_view s) { value += s; }
    void reset() { value.clear(); }
    uint32 getLen() const { return value.size(); }
    const char* c_str() const { return value.c_str(); }
    void shrink_to_fit() { value.shrink_to_fit(); }
};
