#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "core.hpp"
#include "typed_scan.hpp"
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
#define CHECK(expression) do { if (!(expression)) throw std::runtime_error( \
    std::string(__FUNCTION__) + ":" + std::to_string(__LINE__) + " failed: " #expression); } while (false)

struct Memory {
    uint8_t* bytes;
    size_t size;
    explicit Memory(size_t count) : bytes(static_cast<uint8_t*>(VirtualAlloc(nullptr, count,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))), size(count) { CHECK(bytes); }
    ~Memory() { VirtualFree(bytes, 0, MEM_RELEASE); }
    uint64_t address(size_t offset = 0) const { return reinterpret_cast<uintptr_t>(bytes + offset); }
    template<class T> void put(size_t offset, T value) { std::memcpy(bytes + offset, &value, sizeof(value)); }
    template<class T> T get(size_t offset = 0) const { T value{}; std::memcpy(&value, bytes + offset, sizeof(value)); return value; }
};
struct File {
    std::wstring path;
    File() {
        wchar_t directory[MAX_PATH]{}, name[MAX_PATH]{};
        CHECK(GetTempPathW(MAX_PATH, directory));
        CHECK(GetTempFileNameW(directory, L"cep", 0, name));
        path = name;
    }
    ~File() { DeleteFileW(path.c_str()); }
    void put(const std::string& text) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(file != INVALID_HANDLE_VALUE);
        DWORD written = 0;
        const BOOL okay = WriteFile(file, text.data(), DWORD(text.size()), &written, nullptr);
        CloseHandle(file);
        CHECK(okay && written == text.size());
    }
    std::string get() const {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(file != INVALID_HANDLE_VALUE);
        LARGE_INTEGER size{};
        const BOOL measured = GetFileSizeEx(file, &size);
        if (!measured || size.QuadPart < 0 || size.QuadPart > 8 * 1024 * 1024) { CloseHandle(file); CHECK(false); }
        std::string bytes(size_t(size.QuadPart), '\0');
        DWORD received = 0;
        const BOOL okay = ReadFile(file, bytes.data(), DWORD(bytes.size()), &received, nullptr);
        CloseHandle(file);
        CHECK(okay && received == bytes.size());
        return bytes;
    }
};
CeScanRequestV2 request(const Memory& memory, uint32_t comparison = CE_CMP_EXACT, const wchar_t* value = L"7") {
    return {sizeof(CeScanRequestV2), CE_V2_VERSION, CE_TYPE_U32, CE_VALUE_SIGNED,
        comparison, 4, 4, 0, memory.address(), memory.address() + memory.size, value, nullptr};
}
CeScanInfoV2 info(ce::Core& core) {
    CeScanInfoV2 value{};
    CHECK(core.scan_info_v2(value) == CE_OK);
    return value;
}
void valid_status(const CeScanStatusV2& status) {
    CHECK(status.size == sizeof(status) && status.version == CE_V2_VERSION);
    CHECK(!status.reserved1 && !status.reserved2);
    CHECK(status.phase <= CE_SCAN_FAILED && status.unit <= CE_SCAN_UNIT_CANDIDATES);
    CHECK(status.active <= 1 && status.has_scan <= 1 && status.can_undo <= 1);
    CHECK(status.cancel_requested <= 1 && status.total_known <= 1);
    CHECK(status.processed <= status.total);
    CHECK(!status.can_undo || status.has_scan);
    if (!status.total_known) CHECK(!status.processed && !status.total);
    if (status.active) CHECK(status.phase >= CE_SCAN_ENUMERATING && status.phase <= CE_SCAN_COMMITTING);
    if (status.phase >= CE_SCAN_COMPLETED) CHECK(!status.active);
    if (status.phase == CE_SCAN_COMPLETED) CHECK(status.last_status == CE_OK);
    if (status.phase == CE_SCAN_CANCELLED) CHECK(status.last_status == CE_CANCELLED && status.cancel_requested);
}
CeScanStatusV2 status(ce::Core& core) {
    CeScanStatusV2 value{};
    CHECK(core.scan_status_v2(value) == CE_OK);
    valid_status(value);
    return value;
}
uint64_t add(ce::Core& core, uint64_t address, const wchar_t* value, const wchar_t* description,
    const CeAddressV2* pointer = nullptr) {
    CeRecordRequestV2 record{};
    record.size = sizeof(record);
    record.version = CE_V2_VERSION;
    record.type = CE_TYPE_U32;
    record.flags = CE_VALUE_SIGNED;
    record.byte_length = 4;
    record.address.base = address;
    if (pointer) record.address = *pointer;
    record.description = description;
    record.value = value;
    uint64_t id = 0;
    CHECK(core.upsert_record_v2(record, id) == CE_OK && id);
    return id;
}
CeRecordInfoV2 record(ce::Core& core, uint64_t index) {
    CeRecordInfoV2 value{};
    std::wstring description;
    CHECK(core.record_v2(index, value, description) == CE_OK);
    return value;
}

void test_frozen_pointer_write_preflight() {
    ce::Core core;
    Memory memory(4096);
    memory.put<int32_t>(0, 111);
    memory.put<int32_t>(64, 222);
    const uint64_t first = add(core, memory.address(), L"111", L"absolute X");
    uintptr_t pointer = uintptr_t(memory.address(64));
    CeAddressV2 target{};
    target.base = reinterpret_cast<uintptr_t>(&pointer);
    target.offset_count = 1;
    const uint64_t second = add(core, 0, L"222", L"pointer Y", &target);
    CHECK(core.freeze_record_v2(first, true) == CE_OK);
    CHECK(core.freeze_record_v2(second, true) == CE_OK);

    // No freeze tick between retarget and explicit write: this is the regression.
    pointer = uintptr_t(memory.address());
    CHECK(core.write_record_v2(second, L"999") == CE_INVALID_ARGUMENT);
    CHECK(memory.get<int32_t>() == 111 && memory.get<int32_t>(64) == 222);
    const auto rejected = record(core, 1);
    CHECK(rejected.id == second && !rejected.frozen && rejected.last_status == CE_INVALID_ARGUMENT);
    CHECK(record(core, 0).frozen);

    // A rejected write must not replace the saved bytes with 999.
    pointer = uintptr_t(memory.address(64));
    memory.put<int32_t>(64, 333);
    CHECK(core.freeze_record_v2(second, true) == CE_OK);
    core.tick_freezes();
    CHECK(memory.get<int32_t>(64) == 222 && memory.get<int32_t>() == 111);
    CHECK(core.write_record_v2(second, L"444") == CE_OK);
    memory.put<int32_t>(64, 555);
    core.tick_freezes();
    CHECK(memory.get<int32_t>(64) == 444);

    // Ordinary non-frozen writes retain the historical explicit-write behavior.
    CHECK(core.freeze_record_v2(second, false) == CE_OK);
    pointer = uintptr_t(memory.address());
    CHECK(core.write_record_v2(second, L"777") == CE_OK);
    CHECK(memory.get<int32_t>() == 777);
    core.tick_freezes();
    CHECK(memory.get<int32_t>() == 111);
}
void test_duplicate_v1_export_preflight() {
    ce::Core core;
    Memory memory(4096);
    memory.put<int32_t>(0, -12);
    const auto first = add(core, memory.address(), L"-12", L"first duplicate");
    const auto second = add(core, memory.address(), L"-34", L"second duplicate");
    CHECK(first != second);
    File destination;
    const std::string sentinel = "preserve destination exactly\r\n";
    destination.put(sentinel);
    CHECK(core.save_table(destination.path.c_str()) == CE_UNSUPPORTED);
    CHECK(ce::last_error().find(L"V2") != std::wstring::npos);
    CHECK(destination.get() == sentinel);
    uint64_t count = 0;
    CHECK(core.record_count_v2(count) == CE_OK && count == 2);
    CHECK(core.save_table_v2(destination.path.c_str()) == CE_OK);
    ce::Core restored;
    CHECK(restored.load_table_v2(destination.path.c_str()) == CE_OK);
    CHECK(restored.record_count_v2(count) == CE_OK && count == 2);
    for (uint64_t index = 0; index < count; ++index) {
        CeRecordInfoV2 current{};
        std::wstring description;
        CHECK(restored.record_v2(index, current, description) == CE_OK);
        CHECK(current.id == (index ? second : first));
        CHECK(current.resolved_address == memory.address() && !current.frozen);
        CHECK(current.type == CE_TYPE_U32 && current.flags == CE_VALUE_SIGNED && current.byte_length == 4);
        CHECK(description == (index ? L"second duplicate" : L"first duplicate"));
    }
    const auto v2_bytes = destination.get();
    CHECK(restored.save_table(destination.path.c_str()) == CE_UNSUPPORTED);
    CHECK(destination.get() == v2_bytes);
    CHECK(DeleteFileW(destination.path.c_str()));
    CHECK(core.save_table(destination.path.c_str()) == CE_UNSUPPORTED);
    CHECK(GetFileAttributesW(destination.path.c_str()) == INVALID_FILE_ATTRIBUTES);
}
void test_status_history_and_zero_results() {
    ce::Core core;
    Memory memory(64);
    auto current = status(core);
    CHECK(current.phase == CE_SCAN_IDLE && !current.active && !current.operation_id && !current.generation);
    CHECK(!current.has_scan && !current.can_undo && current.unit == CE_SCAN_UNIT_NONE);
    CHECK(core.new_scan_v2() == CE_OK);
    current = status(core);
    CHECK(!current.operation_id && !current.has_scan && current.generation == 1);
    CHECK(core.undo_scan_v2() == CE_INVALID_ARGUMENT);
    CHECK(status(core).operation_id == 0);

    auto scan = request(memory);
    CHECK(core.first_scan_v2(scan) == CE_OK);
    const auto first = status(core);
    CHECK(first.operation_id == 1 && first.generation == 2 && first.has_scan && !first.can_undo);
    CHECK(first.phase == CE_SCAN_COMPLETED && first.unit == CE_SCAN_UNIT_BYTES && first.total_known);
    CHECK(first.processed == memory.size && first.total == memory.size && !info(core).count);
    CHECK(core.next_scan_v2(scan) == CE_OK);
    const auto next = status(core);
    CHECK(next.operation_id == 2 && next.has_scan && next.can_undo && next.phase == CE_SCAN_COMPLETED);
    CHECK(next.unit == CE_SCAN_UNIT_CANDIDATES && next.total_known && !next.processed && !next.total);
    CHECK(core.undo_scan_v2() == CE_OK);
    const auto undone = status(core);
    CHECK(undone.operation_id == next.operation_id && undone.generation == next.generation + 1);
    CHECK(undone.has_scan && !undone.can_undo && !info(core).count);
    CHECK(core.new_scan_v2() == CE_OK);
    current = status(core);
    CHECK(current.operation_id == next.operation_id && !current.has_scan && !current.can_undo);
    CHECK(current.phase == CE_SCAN_IDLE && !current.total_known && !current.processed && !current.total);

    memory.put<int32_t>(0, 7);
    CHECK(core.first_scan_v2(scan) == CE_OK && info(core).count == 1);
    CHECK(core.next_scan_v2(scan) == CE_OK);
    current = status(core);
    CHECK(current.operation_id == 4 && current.processed == 1 && current.total == 1 && current.can_undo);
    const auto committed = info(core);
    scan.version = 99;
    CHECK(core.first_scan_v2(scan) == CE_INVALID_ARGUMENT);
    const auto failed = status(core);
    CHECK(failed.operation_id == current.operation_id + 1 && failed.phase == CE_SCAN_FAILED);
    CHECK(failed.last_status == CE_INVALID_ARGUMENT && !failed.active && failed.has_scan && failed.can_undo);
    CHECK(failed.generation == committed.generation && info(core).count == committed.count);
    scan.version = CE_V2_VERSION;
    scan.type = CE_TYPE_U64;
    scan.byte_length = 8;
    CHECK(core.next_scan_v2(scan) == CE_INVALID_ARGUMENT);
    CHECK(status(core).phase == CE_SCAN_FAILED && status(core).operation_id == failed.operation_id + 1);
    CHECK(info(core).generation == committed.generation && info(core).count == 1);
    CHECK(core.undo_scan_v2() == CE_OK && !status(core).can_undo);

    ce::Core empty;
    scan = request(memory);
    CHECK(empty.next_scan_v2(scan) == CE_INVALID_ARGUMENT);
    current = status(empty);
    CHECK(current.operation_id == 1 && current.phase == CE_SCAN_FAILED && !current.has_scan);
}
void test_readable_range_and_progress_callback() {
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const size_t page = system.dwPageSize;
    Memory memory(3 * page);
    DWORD old = 0;
    CHECK(VirtualProtect(memory.bytes + page, page, PAGE_NOACCESS, &old));
    ce::Core core;
    auto scan = request(memory);
    CHECK(core.first_scan_v2(scan) == CE_OK);
    const auto completed = status(core);
    CHECK(completed.total == 2 * page && completed.processed == completed.total);

    std::vector<ce::typed::ScanProgress> progress;
    std::unique_ptr<ce::typed::ScanFile> output;
    CHECK(ce::typed::scan(scan, nullptr, [] { return false; }, output,
        [&](const auto& value) { progress.push_back(value); }) == CE_OK);
    CHECK(progress.size() == 5); // enumeration, known total, two readable pages, final flush
    CHECK(progress.front().phase == CE_SCAN_ENUMERATING && !progress.front().total_known);
    CHECK(progress[1].phase == CE_SCAN_READING && progress[1].total == 2 * page && !progress[1].processed);
    CHECK(progress[2].processed == page && progress[3].processed == 2 * page);
    CHECK(progress.back().phase == CE_SCAN_WRITING && progress.back().processed == 2 * page);
    CHECK(VirtualProtect(memory.bytes + page, page, PAGE_READWRITE, &old));
}

struct TemporaryEnvironment {
    const wchar_t* name;
    std::wstring previous;
    bool present = false;
    TemporaryEnvironment(const wchar_t* variable, const std::wstring& value) : name(variable) {
        const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
        if (size) {
            std::vector<wchar_t> buffer(size);
            CHECK(GetEnvironmentVariableW(name, buffer.data(), size) < size);
            previous.assign(buffer.data());
            present = true;
        }
        // Changes this test process only; never persists a user/system variable.
        CHECK(SetEnvironmentVariableW(name, value.c_str()));
    }
    ~TemporaryEnvironment() { SetEnvironmentVariableW(name, present ? previous.c_str() : nullptr); }
};
void test_io_failure_preserves_session() {
    ce::Core core;
    Memory memory(4096);
    memory.put<int32_t>(0, 7);
    auto scan = request(memory);
    CHECK(core.first_scan_v2(scan) == CE_OK);
    CHECK(core.next_scan_v2(scan) == CE_OK);
    const auto before = status(core);
    const auto committed = info(core);
    File not_a_directory;
    not_a_directory.put("temporary path blocker");
    {
        // No worker threads run in this scope; both values are restored before
        // any other test or scan uses the process temporary directory again.
        TemporaryEnvironment temporary(L"TEMP", not_a_directory.path);
        TemporaryEnvironment alternate(L"TMP", not_a_directory.path);
        CHECK(core.first_scan_v2(scan) == CE_IO_ERROR);
    }
    const auto failed = status(core);
    CHECK(failed.operation_id == before.operation_id + 1 && failed.phase == CE_SCAN_FAILED);
    CHECK(!failed.active && failed.last_status == CE_IO_ERROR && failed.has_scan && failed.can_undo);
    CHECK(failed.generation == committed.generation && info(core).count == committed.count);
    CHECK(not_a_directory.get() == "temporary path blocker");
    CHECK(core.undo_scan_v2() == CE_OK && info(core).count == 1);
    CHECK(core.next_scan_v2(scan) == CE_OK && status(core).phase == CE_SCAN_COMPLETED);
}

struct Worker {
    ce::Core& core;
    std::atomic<bool> done{false};
    std::atomic<int> result{CE_INTERNAL_ERROR};
    std::thread thread;
    Worker(ce::Core& owner, CeScanRequestV2 scan, bool next) : core(owner), thread([this, scan, next] {
        result.store(next ? core.next_scan_v2(scan) : core.first_scan_v2(scan));
        done.store(true);
    }) {}
    ~Worker() { if (thread.joinable()) { core.cancel_scan(); thread.join(); } }
    void join() { thread.join(); }
};
void test_concurrent_status_cancel_preserves_session() {
    ce::Core core;
    Memory small(4096), large(64 * 1024 * 1024);
    small.put<int32_t>(0, 7);
    auto committed_request = request(small);
    auto scan = request(large, CE_CMP_UNKNOWN, nullptr);
    scan.type = CE_TYPE_U8;
    scan.flags = 0;
    scan.alignment = 1;
    scan.byte_length = 1;
    bool saw_cancelled = false;
    for (unsigned attempt = 0; attempt < 4 && !saw_cancelled; ++attempt) {
        CHECK(core.first_scan_v2(committed_request) == CE_OK);
        CHECK(core.next_scan_v2(committed_request) == CE_OK);
        const auto committed = info(core);
        const auto before = status(core);
        Worker worker(core, scan, false);
        bool requested_cancel = false;
        uint64_t observed_operation = before.operation_id, last_processed = 0;
        const auto deadline = GetTickCount64() + 15000;
        while (!worker.done.load() && GetTickCount64() < deadline) {
            CeScanStatusV2 snapshot{};
            std::memset(&snapshot, 0x5a, sizeof(snapshot));
            const auto sentinel = snapshot;
            const auto start = std::chrono::steady_clock::now();
            const int polled = core.scan_status_v2(snapshot);
            const auto elapsed = std::chrono::steady_clock::now() - start;
            CHECK(elapsed < std::chrono::milliseconds(500));
            CHECK(polled == CE_OK || polled == CE_BUSY);
            if (polled == CE_BUSY) {
                CHECK(std::memcmp(&snapshot, &sentinel, sizeof(snapshot)) == 0);
                if (requested_cancel) core.cancel_scan();
                std::this_thread::yield();
                continue;
            }
            valid_status(snapshot);
            CHECK(snapshot.operation_id >= observed_operation);
            if (snapshot.operation_id != observed_operation) last_processed = 0;
            observed_operation = snapshot.operation_id;
            CHECK(snapshot.processed >= last_processed);
            last_processed = snapshot.processed;
            if (snapshot.active) {
                CHECK(snapshot.operation_id == before.operation_id + 1);
                CHECK(snapshot.has_scan && snapshot.can_undo && snapshot.generation == committed.generation);
                // A snapshot cannot reserve admission: do not issue New/Undo or
                // another scan expecting BUSY after this snapshot has gone stale.
                if (snapshot.phase == CE_SCAN_READING && snapshot.processed) {
                    CHECK(snapshot.total_known && snapshot.unit == CE_SCAN_UNIT_BYTES && snapshot.total == large.size);
                    requested_cancel = true;
                }
            }
            if (requested_cancel) core.cancel_scan();
            std::this_thread::yield();
        }
        CHECK(worker.done.load());
        worker.join();
        const int outcome = worker.result.load();
        const auto terminal = status(core);
        CHECK(terminal.operation_id == before.operation_id + 1);
        CHECK(outcome == CE_OK || outcome == CE_CANCELLED);
        if (outcome == CE_OK) {
            // The worker may commit between an active snapshot and cancellation,
            // or finish before this thread is scheduled. Both are valid success.
            CHECK(terminal.phase == CE_SCAN_COMPLETED && terminal.last_status == CE_OK);
            CHECK(terminal.has_scan && terminal.can_undo && terminal.generation == committed.generation + 1);
            CHECK(terminal.total_known && terminal.processed == terminal.total && terminal.total == large.size);
            CHECK(info(core).count == large.size);
            continue; // Rebuild the small committed session and retry cancellation.
        }
        CHECK(requested_cancel && terminal.phase == CE_SCAN_CANCELLED);
        CHECK(terminal.has_scan && terminal.can_undo && terminal.generation == committed.generation);
        // Source reading can reach total before a cancelled flush/commit.
        CHECK(terminal.processed <= terminal.total && info(core).count == committed.count);
        CeResultV2 result{};
        std::vector<uint8_t> bytes;
        CHECK(core.result_v2(committed.generation, 0, result, bytes) == CE_OK && result.address == small.address());
        saw_cancelled = true;

        // An old cancellation epoch is not sticky for a later admitted pass.
        CHECK(core.next_scan_v2(committed_request) == CE_OK);
        const auto recovered = status(core);
        CHECK(recovered.operation_id == terminal.operation_id + 1 && recovered.phase == CE_SCAN_COMPLETED);
        CHECK(!recovered.cancel_requested && recovered.processed == recovered.total && recovered.total == 1);
    }
    CHECK(saw_cancelled);
    core.request_stop();
    CHECK(core.first_scan_v2(committed_request) == CE_NOT_RUNNING);
    CHECK(core.new_scan_v2() == CE_NOT_RUNNING && core.undo_scan_v2() == CE_NOT_RUNNING);
    CeScanStatusV2 stopped{};
    CHECK(core.scan_status_v2(stopped) == CE_NOT_RUNNING);
}
void test_next_progress_and_cancellation() {
    ce::Core core;
    Memory memory(1024 * 1024);
    bool saw_cancelled = false;
    for (unsigned attempt = 0; attempt < 4 && !saw_cancelled; ++attempt) {
        CHECK(core.new_scan_v2() == CE_OK);
        auto scan = request(memory, CE_CMP_UNKNOWN, nullptr);
        CHECK(core.first_scan_v2(scan) == CE_OK);
        const auto first = info(core);
        CHECK(first.count == memory.size / 4);
        const auto before = status(core);
        scan.comparison = CE_CMP_UNCHANGED;
        Worker worker(core, scan, true);
        bool requested_cancel = false;
        const auto deadline = GetTickCount64() + 15000;
        while (!worker.done.load() && GetTickCount64() < deadline) {
            CeScanStatusV2 snapshot{};
            const int polled = core.scan_status_v2(snapshot);
            CHECK(polled == CE_OK || polled == CE_BUSY);
            if (polled == CE_OK) {
                valid_status(snapshot);
                if (snapshot.active && snapshot.total_known && snapshot.processed) {
                    CHECK(snapshot.operation_id == before.operation_id + 1);
                    CHECK(snapshot.unit == CE_SCAN_UNIT_CANDIDATES && snapshot.total == first.count);
                    CHECK(snapshot.generation == first.generation);
                    requested_cancel = true;
                }
            }
            if (requested_cancel) core.cancel_scan();
            std::this_thread::yield();
        }
        CHECK(worker.done.load());
        worker.join();
        const int outcome = worker.result.load();
        const auto terminal = status(core);
        CHECK(terminal.operation_id == before.operation_id + 1);
        CHECK(outcome == CE_OK || outcome == CE_CANCELLED);
        if (outcome == CE_OK) {
            CHECK(terminal.phase == CE_SCAN_COMPLETED && terminal.last_status == CE_OK);
            CHECK(terminal.has_scan && terminal.can_undo && terminal.generation == first.generation + 1);
            CHECK(terminal.unit == CE_SCAN_UNIT_CANDIDATES && terminal.total_known);
            CHECK(terminal.processed == terminal.total && terminal.total == first.count);
            CHECK(info(core).count == first.count);
            continue;
        }
        CHECK(requested_cancel && terminal.phase == CE_SCAN_CANCELLED);
        CHECK(terminal.has_scan && !terminal.can_undo);
        CHECK(terminal.generation == first.generation && info(core).count == first.count);
        CHECK(terminal.processed <= terminal.total);
        saw_cancelled = true;
    }
    CHECK(saw_cancelled);
}
} // namespace

int main() {
    try {
        test_frozen_pointer_write_preflight();
        test_duplicate_v1_export_preflight();
        test_status_history_and_zero_results();
        test_readable_range_and_progress_callback();
        test_io_failure_preserves_session();
        test_concurrent_status_cancel_preserves_session();
        test_next_progress_and_cancellation();
        std::cout << "All polish core tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::wcerr << ce::last_error() << '\n';
        return 1;
    }
}
