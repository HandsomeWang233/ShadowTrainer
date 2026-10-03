#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "core.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
#define CHECK(expression) do { if (!(expression)) throw std::runtime_error(\
    std::string(__FUNCTION__) + ":" + std::to_string(__LINE__) + " failed: " #expression); } while (false)

struct Memory {
    unsigned char* data = nullptr;
    size_t size = 0;
    explicit Memory(size_t bytes) : size(bytes) {
        data = static_cast<unsigned char*>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        CHECK(data != nullptr);
    }
    ~Memory() { if (data) VirtualFree(data, 0, MEM_RELEASE); }
    Memory(const Memory&) = delete;
    Memory& operator=(const Memory&) = delete;
    uint64_t address(size_t offset = 0) const { return reinterpret_cast<uintptr_t>(data + offset); }
    void put(size_t offset, int32_t value) { CHECK(offset + sizeof(value) <= size); std::memcpy(data + offset, &value, sizeof(value)); }
    int32_t get(size_t offset = 0) const { int32_t result = 0; std::memcpy(&result, data + offset, sizeof(result)); return result; }
};
size_t page_size() { SYSTEM_INFO info{}; GetSystemInfo(&info); return info.dwPageSize; }
CeScanRequest request(const Memory& memory, int32_t value, uint32_t alignment = 4) {
    return {sizeof(CeScanRequest), alignment, memory.address(), memory.address() + memory.size, value, 0};
}
uint64_t count(ce::Core& core) { uint64_t result = 0; CHECK(core.result_count(result) == CE_OK); return result; }
CeResult result_at(ce::Core& core, uint64_t index) { CeResult result{}; CHECK(core.result(index, result) == CE_OK); return result; }
std::wstring hex(uint64_t address) {
    wchar_t buffer[17]{};
    const wchar_t digits[] = L"0123456789ABCDEF";
    size_t position = 16;
    do { buffer[--position] = digits[address & 15]; address >>= 4; } while (address);
    return buffer + position;
}
std::string ascii(const std::wstring& value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value) {
        CHECK(character <= 0x7f);
        result.push_back(static_cast<char>(character));
    }
    return result;
}
struct TableFile {
    std::wstring path;
    TableFile() {
        wchar_t directory[MAX_PATH + 1]{};
        wchar_t name[MAX_PATH + 1]{};
        CHECK(GetTempPathW(MAX_PATH, directory) != 0);
        CHECK(GetTempFileNameW(directory, L"cet", 0, name) != 0);
        path = name;
    }
    ~TableFile() { DeleteFileW(path.c_str()); }
    void put(const std::string& text) {
        const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(file != INVALID_HANDLE_VALUE);
        DWORD written = 0;
        const bool okay = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) != FALSE;
        CloseHandle(file);
        CHECK(okay && written == text.size());
    }
    std::string get() {
        const auto file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(file != INVALID_HANDLE_VALUE);
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size) || size.QuadPart > 16 * 1024 * 1024) { CloseHandle(file); CHECK(false); }
        std::string content(static_cast<size_t>(size.QuadPart), '\0');
        DWORD received = 0;
        const bool okay = ReadFile(file, content.data(), static_cast<DWORD>(content.size()), &received, nullptr) != FALSE;
        CloseHandle(file);
        CHECK(okay && received == content.size());
        return content;
    }
};
std::string table(const std::string& entries, const std::string& suffix = "") {
    return "<?xml version=\"1.0\" encoding=\"utf-8\"?><CheatTable CheatEngineTableVersion=\"45\"><CheatEntries>" +
        entries + "</CheatEntries>" + suffix + "</CheatTable>";
}
std::string entry(uint64_t address, const std::string& extra = "") {
    return "<CheatEntry><Description>&quot;Health &amp; stamina&quot;</Description><VariableType>4 Bytes</VariableType><Address>" +
        ascii(hex(address)) + "</Address>" + extra + "</CheatEntry>";
}
void test_read_write_and_records() {
    ce::Core core;
    Memory memory(page_size() * 2);
    memory.put(0, -12);
    int32_t value = 0;
    CHECK(core.read(memory.address(), value) == CE_OK && value == -12);
    CHECK(core.write(memory.address(), -2147483647) == CE_OK);
    CHECK(memory.get() == -2147483647);
    auto records = core.records();
    CHECK(records.size() == 1 && records[0].value == -2147483647 && !records[0].frozen);
    CHECK(core.write(memory.address(), 42) == CE_OK);
    CHECK(core.records().size() == 1 && memory.get() == 42);
    value = 987;
    CHECK(core.read(0, value) == CE_INVALID_ARGUMENT && value == 987);
    CHECK(core.write((std::numeric_limits<uint64_t>::max)() - 1, 9) == CE_INVALID_ARGUMENT);
    CHECK(core.freeze(0, 9, true) == CE_INVALID_ARGUMENT);
    CHECK(!ce::last_error().empty());
    DWORD old = 0;
    CHECK(VirtualProtect(memory.data + page_size(), page_size(), PAGE_READONLY, &old));
    CHECK(core.write(memory.address(page_size()), 3) == CE_ACCESS_ERROR);
    CHECK(core.write(memory.address(page_size() - 2), 3) == CE_ACCESS_ERROR);
    CHECK(core.records().size() == 1);
    CHECK(core.freeze(memory.address(page_size()), 3, true) == CE_ACCESS_ERROR);
    CHECK(VirtualProtect(memory.data + page_size(), page_size(), PAGE_NOACCESS, &old));
    CHECK(core.read(memory.address(page_size()), value) == CE_ACCESS_ERROR);
    CHECK(core.read(memory.address(page_size() - 2), value) == CE_ACCESS_ERROR);
    CHECK(VirtualProtect(memory.data + page_size(), page_size(), PAGE_READWRITE, &old));
}
void test_first_next_and_alignment() {
    ce::Core core;
    Memory memory(page_size() * 2);
    std::memset(memory.data, 0x7e, memory.size);
    constexpr int32_t target = -123456789;
    memory.put(4, target);
    memory.put(21, target);
    memory.put(64, target);
    CHECK(core.next_scan(target) == CE_INVALID_ARGUMENT);
    CHECK(core.first_scan(request(memory, target, 4)) == CE_OK);
    CHECK(count(core) == 2);
    CHECK(result_at(core, 0).address == memory.address(4));
    CHECK(result_at(core, 1).address == memory.address(64));
    CHECK(result_at(core, 0).value == target && result_at(core, 0).reserved == 0);
    CHECK(core.records().empty());
    memory.put(4, 23);
    CHECK(core.next_scan(23) == CE_OK);
    CHECK(count(core) == 1 && result_at(core, 0).address == memory.address(4));
    CHECK(core.first_scan(request(memory, target, 1)) == CE_OK);
    CHECK(count(core) == 2 && result_at(core, 0).address == memory.address(21));
    auto invalid = request(memory, target);
    invalid.alignment = 2;
    CHECK(core.first_scan(invalid) == CE_INVALID_ARGUMENT && count(core) == 2);
    invalid = request(memory, target);
    invalid.reserved = 1;
    CHECK(core.first_scan(invalid) == CE_INVALID_ARGUMENT && count(core) == 2);
    invalid = request(memory, target);
    invalid.end = invalid.begin;
    CHECK(core.first_scan(invalid) == CE_INVALID_ARGUMENT && count(core) == 2);
    CeResult unchanged{77, 88, 99};
    CHECK(core.result((std::numeric_limits<uint64_t>::max)(), unchanged) == CE_INVALID_ARGUMENT);
    CHECK(unchanged.address == 77 && unchanged.value == 88 && unchanged.reserved == 99);
    // Alignment is absolute address alignment, not relative to begin.
    auto odd_begin = request(memory, target, 4);
    odd_begin.begin += 1;
    CHECK(core.first_scan(odd_begin) == CE_OK && count(core) == 1);
    CHECK(result_at(core, 0).address == memory.address(64));
    auto short_range = request(memory, target, 1);
    short_range.begin = memory.address(21);
    short_range.end = short_range.begin + 3;
    CHECK(core.first_scan(short_range) == CE_OK && count(core) == 0);
    // Explicit stack memory is scanned even though broad scans exclude the scanner stack.
    int32_t local = target;
    CeScanRequest stack{sizeof(CeScanRequest), 1, reinterpret_cast<uintptr_t>(&local), reinterpret_cast<uintptr_t>(&local) + 4, target, 0};
    CHECK(core.first_scan(stack) == CE_OK && count(core) == 1);
}
void test_page_boundaries_and_removal() {
    ce::Core core;
    const auto page = page_size();
    Memory memory(page * 3);
    std::memset(memory.data, 0x71, memory.size);
    constexpr int32_t target = -56789123;
    memory.put(page - 2, target);
    memory.put(page * 2 + 8, target);
    DWORD old = 0;
    CHECK(VirtualProtect(memory.data + page, page, PAGE_READONLY, &old));
    CHECK(core.first_scan(request(memory, target, 1)) == CE_OK);
    CHECK(count(core) == 2);
    CHECK(result_at(core, 0).address == memory.address(page - 2));
    CHECK(result_at(core, 1).address == memory.address(page * 2 + 8));
    CHECK(VirtualFree(memory.data + page, page, MEM_DECOMMIT));
    CHECK(core.next_scan(target) == CE_OK);
    CHECK(count(core) == 1 && result_at(core, 0).address == memory.address(page * 2 + 8));
    CHECK(core.first_scan(request(memory, target, 1)) == CE_OK);
    CHECK(count(core) == 1);
    CHECK(VirtualAlloc(memory.data + page, page, MEM_COMMIT, PAGE_READWRITE) == memory.data + page);
    memory.put(page - 2, target);
    CHECK(VirtualProtect(memory.data + page, page, PAGE_READWRITE | PAGE_GUARD, &old));
    CHECK(core.first_scan(request(memory, target, 1)) == CE_OK && count(core) == 1);
    CHECK(VirtualProtect(memory.data + page, page, PAGE_READWRITE, &old));
}
void test_file_backed_large_results() {
    ce::Core core;
    Memory memory(8 * 1024 * 1024);
    const uint64_t expected = memory.size / sizeof(int32_t);
    CHECK(core.first_scan(request(memory, 0, 4)) == CE_OK);
    CHECK(count(core) == expected);
    CHECK(result_at(core, 0).address == memory.address());
    CHECK(result_at(core, expected / 2).address == memory.address(memory.size / 2));
    CHECK(result_at(core, expected - 1).address == memory.address(memory.size - 4));
    memory.put(memory.size - 4, -777);
    CHECK(core.next_scan(-777) == CE_OK && count(core) == 1);
    CHECK(result_at(core, 0).address == memory.address(memory.size - 4));
}
void test_cancel_busy_and_previous_commit() {
    ce::Core core;
    Memory small(page_size());
    std::memset(small.data, 0x65, small.size);
    small.put(16, -4321);
    CHECK(core.first_scan(request(small, -4321)) == CE_OK && count(core) == 1);
    const auto previous = result_at(core, 0);
    // Dense alignment-1 zeros stream roughly 4 GiB if allowed to finish. The
    // fixed input allocation is intentionally committed before the worker starts.
    Memory large(256 * 1024 * 1024);
    std::atomic<bool> entered{false};
    std::atomic<bool> done{false};
    std::atomic<int> scan_status{CE_INTERNAL_ERROR};
    std::thread worker([&] {
        entered.store(true);
        scan_status.store(core.first_scan(request(large, 0, 1)));
        done.store(true);
    });
    while (!entered.load()) std::this_thread::yield();
    bool busy = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done.load() && std::chrono::steady_clock::now() < deadline) {
        // Observe admission first. Competing for the scan mutex before the
        // worker starts can reject the worker itself with BUSY.
        CeScanStatusV2 observed{}; observed.size = sizeof(observed); observed.version = CE_V2_VERSION;
        if (core.scan_status_v2(observed) == CE_OK && observed.active) {
            CeScanRequest invalid{};
            const auto start = std::chrono::steady_clock::now();
            const auto status = core.first_scan(invalid);
            if (status == CE_BUSY) {
                busy = std::chrono::steady_clock::now() - start < std::chrono::seconds(1);
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto during = count(core);
    for (; !done.load(); std::this_thread::sleep_for(std::chrono::milliseconds(1))) core.cancel_scan();
    worker.join();
    CHECK(busy && during == 1);
    CHECK(scan_status.load() == CE_CANCELLED);
    CHECK(count(core) == 1 && result_at(core, 0).address == previous.address);
    CHECK(core.next_scan(-4321) == CE_OK && count(core) == 1);
}
void test_freeze_and_shutdown() {
    ce::Core core;
    Memory memory(page_size() * 2);
    memory.put(0, 1);
    CHECK(core.freeze(memory.address(), -98, true) == CE_OK);
    CHECK(memory.get() == 1); // Scheduling is inert until the runtime tick.
    core.tick_freezes();
    CHECK(memory.get() == -98);
    memory.put(0, 4);
    core.tick_freezes();
    CHECK(memory.get() == -98);
    CHECK(core.freeze(memory.address(), 0, false) == CE_OK);
    memory.put(0, 8);
    core.tick_freezes();
    CHECK(memory.get() == 8);
    CHECK(core.freeze(memory.address(), 73, true) == CE_OK);
    CHECK(core.write(memory.address(), 74) == CE_OK);
    memory.put(0, 9);
    core.tick_freezes();
    CHECK(memory.get() == 74);
    CHECK(core.freeze(memory.address(page_size()), 55, true) == CE_OK);
    CHECK(VirtualFree(memory.data + page_size(), page_size(), MEM_DECOMMIT));
    core.tick_freezes();
    const auto records = core.records();
    CHECK(records.size() == 2 && !records[1].frozen);
    core.shutdown();
    memory.put(0, 88);
    core.tick_freezes();
    CHECK(memory.get() == 88 && core.records().empty());
    CHECK(core.write(memory.address(), 0) == CE_NOT_RUNNING);
    int32_t value = 0;
    CHECK(core.read(memory.address(), value) == CE_NOT_RUNNING);
    CHECK(core.first_scan(request(memory, 0)) == CE_NOT_RUNNING);
    CHECK(core.next_scan(0) == CE_NOT_RUNNING);
    CHECK(core.freeze(memory.address(), 0, true) == CE_NOT_RUNNING);
    uint64_t unchanged = 13;
    CHECK(core.result_count(unchanged) == CE_NOT_RUNNING && unchanged == 13);
    core.shutdown(); // Idempotent.
}
void test_ct_roundtrip_and_rejection() {
    ce::Core core;
    Memory memory(page_size());
    memory.put(0, 123);
    memory.put(8, -456);
    TableFile input, saved;
    input.put(table(entry(memory.address(), "<ID>7</ID>")));
    CHECK(core.load_table(input.path.c_str()) == CE_OK);
    auto records = core.records();
    CHECK(records.size() == 1 && records[0].address == memory.address() && records[0].value == 123 && !records[0].frozen);
    CHECK(core.freeze(memory.address(), 999, true) == CE_OK);
    CHECK(core.save_table(saved.path.c_str()) == CE_OK);
    const auto serialized = saved.get();
    CHECK(serialized.find("Health &amp; stamina") != std::string::npos);
    CHECK(serialized.find("<ID>7</ID>") != std::string::npos);
    CHECK(serialized.find("CheatEngineTableVersion=\"45\"") != std::string::npos);
    CHECK(serialized.find("Frozen") == std::string::npos);
    CHECK(core.load_table(saved.path.c_str()) == CE_OK);
    CHECK(memory.get() == 123 && !core.records()[0].frozen);
    memory.put(0, 321);
    core.tick_freezes();
    CHECK(memory.get() == 321); // Imported tables do not activate saved freezes/values.
    CHECK(core.write(memory.address(8), -567) == CE_OK);
    CHECK(core.save_table(saved.path.c_str()) == CE_OK);
    CHECK(core.load_table(saved.path.c_str()) == CE_OK && core.records().size() == 2);

    const std::vector<std::string> bad{
        table(entry(memory.address(), "<AssemblerScript>anything</AssemblerScript>")),
        table(entry(memory.address(), "<Offsets><Offset>0</Offset></Offsets>")),
        table(entry(memory.address(), "<LastState Value=\"999\" Activated=\"1\"/>")),
        table(entry(memory.address()), "<UserdefinedSymbols/>"),
        table(entry(memory.address()), "<LuaScript>anything</LuaScript>"),
        table(entry(memory.address()), "<Structures/>"),
        table(entry(memory.address()), "<Forms><Form>TPF0</Form></Forms>"),
        table(entry(memory.address()), "<LCL/>"),
        table("<CheatEntry><Description><Nested/></Description><VariableType>4 Bytes</VariableType><Address>" + ascii(hex(memory.address())) + "</Address></CheatEntry>"),
        table("<CheatEntry Unexpected=\"1\"><VariableType>4 Bytes</VariableType><Address>10000</Address></CheatEntry>"),
        table("<CheatEntry><VariableType>Float</VariableType><Address>10000</Address></CheatEntry>"),
        table("<CheatEntry><VariableType>4 Bytes</VariableType><Address>FFFFFFFFFFFFFFFF</Address></CheatEntry>"),
        table("<CheatEntry><VariableType>4 Bytes</VariableType><Address>10000000000000000</Address></CheatEntry>"),
        table("<CheatEntry><VariableType>4 Bytes</VariableType><Address>module.exe+10</Address></CheatEntry>"),
        table("<CheatEntry><VariableType>4 Bytes</VariableType><Address>0</Address></CheatEntry>"),
        table(entry(memory.address(), "<Address>10000</Address>")),
        table(entry(memory.address()) + entry(memory.address())),
        table("<CheatEntry><Address>10000</Address></CheatEntry>"),
        "<CheatTable CheatEngineTableVersion=\"44\"><CheatEntries/></CheatTable>",
        "<!DOCTYPE CheatTable [<!ENTITY x SYSTEM \"file:///C:/Windows/win.ini\">]><CheatTable CheatEngineTableVersion=\"45\"><CheatEntries/></CheatTable>",
        "<CheatTable xmlns=\"urn:unknown\" CheatEngineTableVersion=\"45\"><CheatEntries/></CheatTable>",
        table(entry(memory.address()), "<!-- unknown content -->"),
        table(entry(memory.address()), "<?unknown content?>"),
        table(entry(memory.address()), "<![CDATA[unknown content]]>"),
        table(entry(memory.address())) + "trailing garbage",
        "<CheatTable CheatEngineTableVersion=\"45\"><CheatEntries>"
    };
    CHECK(core.freeze(memory.address(), 321, true) == CE_OK);
    for (const auto& content : bad) {
        input.put(content);
        const auto status = core.load_table(input.path.c_str());
        CHECK(status == CE_INVALID_ARGUMENT || status == CE_UNSUPPORTED);
        CHECK(!ce::last_error().empty());
        records = core.records();
        CHECK(records.size() == 2 && records[0].frozen && records[0].value == 321);
        CHECK(memory.get() == 321 && memory.get(8) == -567);
    }
    input.put(table("<CheatEntry><Description>" + std::string(4097, 'a') + "</Description><VariableType>4 Bytes</VariableType><Address>10000</Address></CheatEntry>"));
    CHECK(core.load_table(input.path.c_str()) == CE_UNSUPPORTED && core.records().size() == 2);
    input.put(std::string(8 * 1024 * 1024 + 1, 'x'));
    CHECK(core.load_table(input.path.c_str()) == CE_UNSUPPORTED && core.records().size() == 2);

    // A sharing violation at the final rename leaves the previous destination intact.
    saved.put("previous destination contents");
    const auto held = CreateFileW(saved.path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(held != INVALID_HANDLE_VALUE);
    const auto save_status = core.save_table(saved.path.c_str());
    CloseHandle(held);
    CHECK(save_status == CE_IO_ERROR && saved.get() == "previous destination contents");
    input.put(table(""));
    CHECK(core.load_table(input.path.c_str()) == CE_OK && core.records().empty());
    CHECK(core.save_table(saved.path.c_str()) == CE_OK);
    CHECK(core.load_table(saved.path.c_str()) == CE_OK && core.records().empty());
}
void test_thread_local_errors() {
    ce::set_error(L"main thread sentinel");
    std::wstring observed;
    std::thread worker([&] {
        CHECK(ce::last_error().empty());
        ce::Core core;
        int32_t value = 0;
        CHECK(core.read(0, value) == CE_INVALID_ARGUMENT);
        observed = ce::last_error();
        ce::set_error(L"worker sentinel");
    });
    worker.join();
    CHECK(!observed.empty());
    CHECK(ce::last_error() == L"main thread sentinel");
}
void test_sticky_stop_rejects_late_scan() {
    ce::Core core;
    Memory memory(4096);
    std::atomic<bool> admitted{false};
    std::atomic<bool> resume{false};
    std::atomic<int> status{CE_INTERNAL_ERROR};
    std::thread worker([&] {
        admitted.store(true);
        while (!resume.load()) std::this_thread::yield();
        status.store(core.first_scan(request(memory, 0, 1)));
    });
    while (!admitted.load()) std::this_thread::yield();
    core.request_stop();
    resume.store(true);
    worker.join();
    CHECK(status.load() == CE_NOT_RUNNING);
    CHECK(core.next_scan(0) == CE_NOT_RUNNING);
    core.shutdown();
}
void test_shutdown_cancels_scan() {
    ce::Core core;
    Memory memory(128 * 1024 * 1024);
    std::atomic<bool> entered{false};
    std::atomic<int> status{CE_INTERNAL_ERROR};
    std::thread worker([&] { entered.store(true); status.store(core.first_scan(request(memory, 0, 1))); });
    while (!entered.load()) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    core.shutdown();
    worker.join();
    CHECK(status.load() == CE_CANCELLED || status.load() == CE_NOT_RUNNING);
    core.tick_freezes();
    CHECK(core.records().empty());
}
} // namespace
int main() {
    try {
        test_read_write_and_records();
        test_first_next_and_alignment();
        test_page_boundaries_and_removal();
        test_file_backed_large_results();
        test_cancel_busy_and_previous_commit();
        test_freeze_and_shutdown();
        test_ct_roundtrip_and_rejection();
        test_thread_local_errors();
        test_sticky_stop_rejects_late_scan();
        test_shutdown_cancels_scan();
        std::cout << "All core tests passed (own-process int32 milestone only).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::wcerr << L"Core diagnostic: " << ce::last_error() << L'\n';
        return 1;
    }
}
