#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// WIN32_LEAN_AND_MEAN keeps windows.h from pulling this in, and it cannot live in
// process_view.inc: the .inc is included inside namespace ce.
#include <tlhelp32.h>
#include <objbase.h>
#include <shlwapi.h>
#include <xmllite.h>
#include "core.hpp"
#include "pointer_scan.hpp"
#include "typed_value.hpp"
#include "typed_scan.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <limits>
#include <mutex>
#include <map>
#include <set>
#include <new>
#include <utility>

namespace ce {
namespace {
thread_local std::wstring diagnostic;
constexpr uint64_t max_table_bytes = 8 * 1024 * 1024;
constexpr size_t max_table_records = 10000;
constexpr size_t max_description_chars = 4096;
// Preserved CT subtrees are re-emitted verbatim, so scripts and structures need far
// more room than a description. The 8 MiB file cap is still the real ceiling.
constexpr size_t max_preserved_chars = 4 * 1024 * 1024;
std::atomic<uint64_t> temporary_serial{0};

int fail(int status, const wchar_t* message) {
    set_error(message);
    return status;
}
int win_fail(const wchar_t* message) {
    const auto code = GetLastError();
    set_error(std::wstring(message) + L" (Win32 " + std::to_wstring(code) + L")");
    return CE_IO_ERROR;
}
template<class F> int guarded(F&& operation) {
    try {
        set_error(L"");
        return operation();
    } catch (const std::bad_alloc&) {
        return fail(CE_INTERNAL_ERROR, L"Insufficient memory; the previous committed state is retained.");
    } catch (...) {
        return fail(CE_INTERNAL_ERROR, L"Unexpected core operation failure.");
    }
}

struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { close(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    void close() noexcept {
        if (value != INVALID_HANDLE_VALUE && value != nullptr) CloseHandle(value);
        value = INVALID_HANDLE_VALUE;
    }
    bool valid() const noexcept { return value != INVALID_HANDLE_VALUE && value != nullptr; }
};
struct TemporaryFile {
    Handle handle;
    std::wstring path;
    ~TemporaryFile() {
        handle.close();
        if (!path.empty()) DeleteFileW(path.c_str());
    }
};

int create_temporary(const std::wstring& prefix, TemporaryFile& file, bool scan_file) {
    for (unsigned attempt = 0; attempt != 128; ++attempt) {
        file.path = prefix + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(temporary_serial.fetch_add(1)) + L".tmp";
        file.handle.value = CreateFileW(file.path.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
            (scan_file ? FILE_ATTRIBUTE_TEMPORARY : FILE_ATTRIBUTE_NORMAL) | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file.handle.valid()) return CE_OK;
        const auto error = GetLastError();
        // Never delete somebody else's colliding file in the RAII destructor.
        file.path.clear();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
            SetLastError(error);
            return win_fail(L"Cannot create a transaction file");
        }
    }
    return fail(CE_IO_ERROR, L"Cannot obtain a unique transaction file name.");
}
bool read_exact(HANDLE file, void* destination, DWORD bytes) {
    auto* output = static_cast<unsigned char*>(destination);
    while (bytes) {
        DWORD received = 0;
        if (!ReadFile(file, output, bytes, &received, nullptr) || !received) return false;
        output += received;
        bytes -= received;
    }
    return true;
}
bool write_exact(HANDLE file, const void* source, DWORD bytes) {
    const auto* input = static_cast<const unsigned char*>(source);
    while (bytes) {
        DWORD written = 0;
        if (!WriteFile(file, input, bytes, &written, nullptr) || !written) return false;
        input += written;
        bytes -= written;
    }
    return true;
}
uint64_t pointer_value(const void* p) { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p)); }

bool readable_protection(DWORD protection) {
    if (protection & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    switch (protection & 0xff) {
    case PAGE_READONLY: case PAGE_READWRITE: case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY: return true;
    default: return false;
    }
}
bool writable_protection(DWORD protection) {
    if (protection & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    switch (protection & 0xff) {
    case PAGE_READWRITE: case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY: return true;
    default: return false;
    }
}
bool valid_address(uint64_t address) {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const auto maximum = pointer_value(info.lpMaximumApplicationAddress);
    return address >= pointer_value(info.lpMinimumApplicationAddress) &&
        address <= maximum && maximum - address >= sizeof(int32_t) - 1 &&
        address <= static_cast<uint64_t>((std::numeric_limits<uintptr_t>::max)()) - (sizeof(int32_t) - 1);
}
bool accessible(uint64_t address, bool writing) {
    if (!valid_address(address)) return false;
    const uint64_t end = address + sizeof(int32_t);
    while (address < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<const void*>(static_cast<uintptr_t>(address)), &info, sizeof(info))) return false;
        if (info.State != MEM_COMMIT || !(writing ? writable_protection(info.Protect) : readable_protection(info.Protect))) return false;
        const uint64_t next = pointer_value(info.BaseAddress) + info.RegionSize;
        if (next <= address) return false;
        address = (std::min)(next, end);
    }
    return true;
}
bool read_value(uint64_t address, int32_t& value) {
    if (!accessible(address, false)) return false;
    int32_t temporary = 0;
    SIZE_T received = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(static_cast<uintptr_t>(address)),
            &temporary, sizeof(temporary), &received) || received != sizeof(temporary)) return false;
    value = temporary;
    return true;
}
bool write_value(uint64_t address, int32_t value) {
    // WPM can otherwise modify read-only mappings on some Windows versions.
    // Do not change protections; validate both pages of an unaligned value.
    if (!accessible(address, true)) return false;
    SIZE_T written = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(static_cast<uintptr_t>(address)),
        &value, sizeof(value), &written) && written == sizeof(value);
}


// A parsed CT element. Attribute order is not preserved (std::map sorts), so a
// re-emitted table is semantically equal to the source, never byte-identical.
struct XmlNode {
    std::wstring name, text;
    std::map<std::wstring, std::wstring> attributes;
    std::vector<XmlNode> children;
};
// Top-level siblings of <CheatEntries> (symbols, structures, forms, Lua) are kept
// so an imported table can be written back without dropping what this build ignores.
struct TableExtra {
    std::vector<XmlNode> nodes;
};

struct StoredRecord {
    AddressRecord record;
    bool has_description = false;
    std::wstring description;
    bool has_id = false;
    std::wstring id;
    uint64_t stable_id = 0;
    uint32_t type = CE_TYPE_U32, flags = CE_VALUE_SIGNED, width = 4;
    CeAddressV2 target{};
    std::vector<uint8_t> raw;
    bool raw_valid = true;
    uint32_t last_status = CE_OK;
    bool ct_signed_unspecified = false;
    std::wstring ct_expression;
    // A CT entry whose VariableType this build cannot represent. It is preserved
    // verbatim, shown read-only, and refused by every edit path.
    bool ct_opaque = false;
    XmlNode ct_raw;
    std::vector<XmlNode> ct_extra;
    bool legacy() const { return !ct_opaque && type == CE_TYPE_U32 && flags == CE_VALUE_SIGNED && width == 4 && !target.offset_count; }
    CeAddressV2 location() const { auto a=target; if(!a.base) a.base=record.address; return a; }
    const void* data() const { return raw.empty() ? static_cast<const void*>(&record.value) : raw.data(); }
};

template<class T> struct ComPointer {
    T* value = nullptr;
    ~ComPointer() { if (value) value->Release(); }
    T** put() { return &value; }
    T* operator->() const { return value; }
};
struct ComScope {
    HRESULT status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComScope() { if (SUCCEEDED(status)) CoUninitialize(); }
    bool valid() const { return SUCCEEDED(status) || status == RPC_E_CHANGED_MODE; }
};
std::wstring trim(const std::wstring& text) {
    const auto first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    return text.substr(first, text.find_last_not_of(L" \t\r\n") - first + 1);
}
bool parse_unsigned(const std::wstring& text, unsigned base, uint64_t& value) {
    if (text.empty()) return false;
    value = 0;
    for (const auto character : text) {
        unsigned digit = 99;
        if (character >= L'0' && character <= L'9') digit = character - L'0';
        else if (character >= L'a' && character <= L'f') digit = character - L'a' + 10;
        else if (character >= L'A' && character <= L'F') digit = character - L'A' + 10;
        if (digit >= base || value > ((std::numeric_limits<uint64_t>::max)() - digit) / base) return false;
        value = value * base + digit;
    }
    return true;
}
std::wstring hexadecimal(uint64_t address) {
    wchar_t buffer[17]{};
    const wchar_t digits[] = L"0123456789ABCDEF";
    size_t position = 16;
    do { buffer[--position] = digits[address & 15]; address >>= 4; } while (address);
    return buffer + position;
}

// Deliberately small, bounded CT grammar. No DTDs, namespaces, CDATA,
// processing instructions, child tables, scripts, expressions, or unknown fields.
// The optional descriptive fields are retained rather than silently discarded.
class TableReader {
public:
    int parse(const std::vector<unsigned char>& bytes, std::vector<StoredRecord>& records) {
        ComScope com;
        if (!com.valid()) return fail(CE_INTERNAL_ERROR, L"Cannot initialize XML services.");
        ComPointer<IStream> stream;
        stream.value = SHCreateMemStream(bytes.data(), static_cast<UINT>(bytes.size()));
        if (!stream.value) return fail(CE_INTERNAL_ERROR, L"Cannot allocate XML input stream.");
        ComPointer<IXmlReader> reader;
        if (FAILED(CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void**>(reader.put()), nullptr)) ||
            FAILED(reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit)) ||
            FAILED(reader->SetProperty(XmlReaderProperty_MaxElementDepth, 8)) ||
            FAILED(reader->SetInput(stream.value))) return fail(CE_INTERNAL_ERROR, L"Cannot configure the strict CT reader.");
        reader_ = reader.value;
        XmlNodeType type = XmlNodeType_None;
        HRESULT status = S_OK;
        while ((status = reader->Read(&type)) == S_OK) {
            if (type == XmlNodeType_XmlDeclaration) {
                if (seen_root_) return invalid();
                continue;
            }
            if (type == XmlNodeType_Whitespace || type == XmlNodeType_Text) {
                const wchar_t* value = nullptr;
                UINT size = 0;
                if (FAILED(reader->GetValue(&value, &size))) return invalid();
                if (field_ != Field::none) {
                    const size_t limit = field_ == Field::description ? max_description_chars : 64;
                    if (text_.size() + size > limit) return unsupported(L"CT field exceeds the supported length.");
                    text_.append(value, size);
                } else if (!trim(std::wstring(value, size)).empty()) return invalid();
                continue;
            }
            if (type == XmlNodeType_Element) {
                std::wstring name;
                if (!name_of_node(name)) return unsupported(L"CT namespaces and prefixed names are not supported.");
                const bool empty = reader->IsEmptyElement() != FALSE;
                if (depth_ == 0) {
                    if (seen_root_ || name != L"CheatTable") return invalid();
                    if (!attributes(true)) return unsupported(L"Only CheatEngineTableVersion=45 is supported on CheatTable.");
                    seen_root_ = true;
                } else {
                    if (!attributes(false)) return unsupported(L"Unexpected CT attributes are not supported.");
                    if (depth_ == 1) {
                        if (name != L"CheatEntries" || seen_entries_) return unsupported(L"Only one CheatEntries container is supported.");
                        seen_entries_ = true;
                    } else if (depth_ == 2) {
                        if (name != L"CheatEntry" || names_[1] != L"CheatEntries") return unsupported(L"Unsupported CT container or record type.");
                        if (records.size() >= max_table_records) return unsupported(L"CT supports at most 10000 records.");
                        current_ = StoredRecord{};
                        fields_ = 0;
                    } else if (depth_ == 3) {
                        if (names_[2] != L"CheatEntry") return invalid();
                        if (name == L"Description") field_ = Field::description;
                        else if (name == L"Address") field_ = Field::address;
                        else if (name == L"VariableType") field_ = Field::variable_type;
                        else if (name == L"ID") field_ = Field::id;
                        else return unsupported(L"Unsupported CT record field (scripts, offsets, values, and activation are not accepted).");
                        const auto bit = 1u << static_cast<unsigned>(field_);
                        if (fields_ & bit) return invalid();
                        fields_ |= bit;
                        text_.clear();
                    } else return unsupported(L"Nested CT fields are not supported.");
                }
                if (depth_ >= names_.size()) return invalid();
                names_[depth_++] = std::move(name);
                if (empty) {
                    const auto result = close_element(records);
                    if (result != CE_OK) return result;
                }
                continue;
            }
            if (type == XmlNodeType_EndElement) {
                std::wstring name;
                if (!name_of_node(name) || !depth_ || names_[depth_ - 1] != name) return invalid();
                const auto result = close_element(records);
                if (result != CE_OK) return result;
                continue;
            }
            return unsupported(L"Unsupported XML content: no DTD, entities, comments, CDATA, or processing instructions.");
        }
        if (FAILED(status) || depth_ || !seen_root_ || !seen_entries_ || !closed_root_) return invalid();
        return CE_OK;
    }
private:
    enum class Field { none, description, address, variable_type, id };
    IXmlReader* reader_ = nullptr;
    std::array<std::wstring, 4> names_;
    size_t depth_ = 0;
    bool seen_root_ = false, seen_entries_ = false, closed_root_ = false;
    Field field_ = Field::none;
    unsigned fields_ = 0;
    std::wstring text_;
    StoredRecord current_;
    int invalid() { return fail(CE_INVALID_ARGUMENT, L"Invalid or incomplete supported CT document; previous records are unchanged."); }
    int unsupported(const wchar_t* text) { return fail(CE_UNSUPPORTED, text); }
    bool name_of_node(std::wstring& name) {
        const wchar_t* text = nullptr;
        UINT size = 0;
        if (FAILED(reader_->GetNamespaceUri(&text, &size)) || size) return false;
        if (FAILED(reader_->GetPrefix(&text, &size)) || size) return false;
        if (FAILED(reader_->GetLocalName(&text, &size))) return false;
        name.assign(text, size);
        return true;
    }
    bool attributes(bool root) {
        UINT count = 0;
        if (FAILED(reader_->GetAttributeCount(&count)) || count != (root ? 1u : 0u)) return false;
        if (!root) return true;
        if (reader_->MoveToFirstAttribute() != S_OK) return false;
        std::wstring name;
        const wchar_t* value = nullptr;
        UINT size = 0;
        const bool valid = name_of_node(name) && name == L"CheatEngineTableVersion" &&
            SUCCEEDED(reader_->GetValue(&value, &size)) && std::wstring(value, size) == L"45";
        return reader_->MoveToElement() == S_OK && valid;
    }
    int close_element(std::vector<StoredRecord>& records) {
        if (!depth_) return invalid();
        if (depth_ == 4) {
            const auto text = trim(text_);
            if (field_ == Field::description) {
                current_.has_description = true;
                current_.description = text_; // Includes ordinary CE's description quotes.
            } else if (field_ == Field::variable_type) {
                if (text != L"4 Bytes") return unsupported(L"Only VariableType 4 Bytes is supported.");
            } else if (field_ == Field::address) {
                std::wstring digits = text;
                if (digits.size() > 2 && digits[0] == L'0' && (digits[1] == L'x' || digits[1] == L'X')) digits.erase(0, 2);
                uint64_t address = 0;
                if (digits.size() > 16 || !parse_unsigned(digits, 16, address) || !valid_address(address))
                    return unsupported(L"Address must be a nonzero, nonoverflowing absolute user-mode hexadecimal int32 address.");
                current_.record.address = address;
            } else if (field_ == Field::id) {
                uint64_t id = 0;
                if (!parse_unsigned(text, 10, id) || id > (std::numeric_limits<uint32_t>::max)()) return invalid();
                current_.has_id = true;
                current_.id = std::to_wstring(id);
            } else return invalid();
            field_ = Field::none;
            text_.clear();
        } else if (depth_ == 3) {
            const auto required = (1u << static_cast<unsigned>(Field::address)) | (1u << static_cast<unsigned>(Field::variable_type));
            if ((fields_ & required) != required) return invalid();
            for (const auto& existing : records) {
                if (existing.record.address == current_.record.address ||
                    (existing.has_id && current_.has_id && existing.id == current_.id)) return invalid();
            }
            // CT carries no value in this subset; an unreadable record stays inert at 0.
            current_.raw_valid = read_value(current_.record.address, current_.record.value);
            current_.last_status = current_.raw_valid ? CE_OK : CE_ACCESS_ERROR;
            current_.record.frozen = false;
            records.push_back(std::move(current_));
        } else if (depth_ == 1) {
            if (!seen_entries_) return invalid();
            closed_root_ = true;
        }
        --depth_;
        return CE_OK;
    }
};

int serialize_table(const std::vector<StoredRecord>& records, IStream* stream) {
    ComPointer<IXmlWriter> writer;
    if (FAILED(CreateXmlWriter(__uuidof(IXmlWriter), reinterpret_cast<void**>(writer.put()), nullptr)) ||
        FAILED(writer->SetOutput(stream)) || FAILED(writer->SetProperty(XmlWriterProperty_Indent, TRUE)))
        return fail(CE_INTERNAL_ERROR, L"Cannot configure the CT writer.");
    HRESULT status = writer->WriteStartDocument(XmlStandalone_Omit);
    auto step = [&](HRESULT next) { if (SUCCEEDED(status)) status = next; };
    step(writer->WriteStartElement(nullptr, L"CheatTable", nullptr));
    step(writer->WriteAttributeString(nullptr, L"CheatEngineTableVersion", nullptr, L"45"));
    step(writer->WriteStartElement(nullptr, L"CheatEntries", nullptr));
    for (const auto& entry : records) {
        step(writer->WriteStartElement(nullptr, L"CheatEntry", nullptr));
        if (entry.has_id) step(writer->WriteElementString(nullptr, L"ID", nullptr, entry.id.c_str()));
        if (entry.has_description) step(writer->WriteElementString(nullptr, L"Description", nullptr, entry.description.c_str()));
        step(writer->WriteElementString(nullptr, L"VariableType", nullptr, L"4 Bytes"));
        const auto address = hexadecimal(entry.record.address);
        step(writer->WriteElementString(nullptr, L"Address", nullptr, address.c_str()));
        step(writer->WriteEndElement());
        if (FAILED(status)) break;
    }
    step(writer->WriteEndElement());
    step(writer->WriteEndElement());
    step(writer->WriteEndDocument());
    step(writer->Flush());
    return FAILED(status) ? fail(CE_IO_ERROR, L"Cannot serialize the supported CT document.") : CE_OK;
}
} // namespace

void set_error(const std::wstring& text) { diagnostic = text; }
const std::wstring& last_error() { return diagnostic; }

struct Core::Impl {
    std::mutex state_mutex;
    std::mutex scan_mutex;
    std::atomic<bool> stopped{false};
    std::atomic<uint64_t> cancellation{0};
    std::vector<StoredRecord> address_records;
    TableExtra table_extra;
    std::unique_ptr<typed::ScanFile> active, undo;
    uint64_t generation = 0, revision = 0, next_record_id = 1;

    // Progress never shares the scan/file-I/O lock. Readers try both mutexes
    // (state, then telemetry) and copy one coherent operation-tagged snapshot.
    std::mutex telemetry_mutex;
    CeScanStatusV2 telemetry{sizeof(CeScanStatusV2), CE_V2_VERSION};
    uint64_t telemetry_epoch = 0;
    // The pointer scan gets its own slot under the same mutex. Reusing the value scan's
    // would make a pointer scan look like a value scan: the UI would freeze the address
    // list for its whole duration and render its phases on the Scan page.
    CePointerScanStatusV2 pointer_telemetry{sizeof(CePointerScanStatusV2), CE_V2_VERSION};
    std::vector<CePointerScanResultV2> pointer_results;
    uint64_t pointer_generation = 0;
    uint64_t pointer_operation = 0;

    void scan_progress(uint64_t operation, const typed::ScanProgress& progress) {
        std::lock_guard lock(telemetry_mutex);
        if (telemetry.operation_id != operation || !telemetry.active) return;
        telemetry.phase = progress.phase;
        telemetry.unit = progress.unit;
        telemetry.total_known = progress.total_known ? 1u : 0u;
        telemetry.processed = progress.processed;
        telemetry.total = progress.total;
    }
    void finish_scan(uint64_t operation, int status) {
        std::lock_guard lock(telemetry_mutex);
        if (telemetry.operation_id != operation || !telemetry.active) return;
        telemetry.active = 0;
        telemetry.last_status = status;
        telemetry.cancel_requested = status == CE_CANCELLED || cancelled(telemetry_epoch);
        telemetry.phase = status == CE_OK ? CE_SCAN_COMPLETED :
            status == CE_CANCELLED ? CE_SCAN_CANCELLED : CE_SCAN_FAILED;
    }
    void reset_scan_telemetry(uint32_t phase) {
        // Caller holds state_mutex and scan_mutex. New/Undo are not scan passes.
        std::lock_guard lock(telemetry_mutex);
        const uint64_t operation = telemetry.operation_id;
        telemetry = {sizeof(CeScanStatusV2), CE_V2_VERSION};
        telemetry.operation_id = operation;
        telemetry.phase = phase;
    }
    struct ScanOperation {
        Impl& owner;
        uint64_t id;
        bool finished = false;
        ~ScanOperation() {
            // guarded() maps exceptions to CE_INTERNAL_ERROR. Do not leave an
            // admitted pass looking active when allocation or I/O setup throws.
            if (!finished) owner.finish_scan(id, CE_INTERNAL_ERROR);
        }
        int finish(int status) {
            owner.finish_scan(id, status);
            finished = true;
            return status;
        }
    };

    bool compatible_scan() const { return !active || (active->info.type == CE_TYPE_U32 && active->info.flags == CE_VALUE_SIGNED && active->info.byte_length == 4); }
    int typed_scan(const CeScanRequestV2& request, bool next_pass, bool legacy) {
        const auto epoch = cancellation.load();
        std::unique_lock scan_lock(scan_mutex, std::try_to_lock);
        if (!scan_lock.owns_lock()) return fail(CE_BUSY, L"Another scan is already running.");
        if (stopped.load()) return stopped_error();
        uint64_t operation_id = 0;
        {
            std::lock_guard lock(telemetry_mutex);
            if (telemetry.operation_id == UINT64_MAX)
                return fail(CE_INTERNAL_ERROR, L"Scan operation IDs exhausted.");
            operation_id = telemetry.operation_id + 1;
            telemetry = {sizeof(CeScanStatusV2), CE_V2_VERSION};
            telemetry.operation_id = operation_id;
            telemetry.active = 1;
            telemetry.phase = next_pass ? CE_SCAN_READING : CE_SCAN_ENUMERATING;
            telemetry.unit = next_pass ? CE_SCAN_UNIT_CANDIDATES : CE_SCAN_UNIT_BYTES;
            telemetry_epoch = epoch;
        }
        ScanOperation operation{*this, operation_id};
        const typed::ScanFile* previous = nullptr;
        {
            std::lock_guard lock(state_mutex);
            if (next_pass && !active)
                return operation.finish(fail(CE_INVALID_ARGUMENT, L"Run a first scan before a next scan."));
            if (next_pass && legacy && !compatible_scan())
                return operation.finish(fail(CE_UNSUPPORTED, L"V1 next scan cannot interpret the active V2 type."));
            if (generation == UINT64_MAX)
                return operation.finish(fail(CE_INTERNAL_ERROR, L"Scan generation exhausted."));
            if (next_pass) previous = active.get();
        }
        // scan_mutex keeps previous alive, while result/status readers remain free.
        std::unique_ptr<typed::ScanFile> pending;
        int status = typed::scan(request, previous, [&] { return cancelled(epoch); }, pending,
            [&](const typed::ScanProgress& progress) { scan_progress(operation_id, progress); });
        if (status == CE_CANCELLED) return operation.finish(cancelled_error());
        if (status != CE_OK) return operation.finish(status);
        {
            std::lock_guard lock(telemetry_mutex);
            telemetry.phase = CE_SCAN_COMMITTING;
        }
        std::lock_guard lock(state_mutex);
        if (cancelled(epoch)) return operation.finish(cancelled_error());
        status = pending->stamp(generation + 1);
        if (status != CE_OK) return operation.finish(status);
        if (cancelled(epoch)) return operation.finish(cancelled_error());
        undo = std::move(active);
        active = std::move(pending);
        ++generation;
        return operation.finish(CE_OK);
    }
    bool conflict(uint64_t address,uint32_t width,uint64_t except) {
        for(auto& item:address_records) { if(!item.record.frozen||item.stable_id==except)continue;
            uint64_t other=0; if(typed::resolve(item.location(),other)!=CE_OK)continue;
            if(address<other+item.width && other<address+width)return true;
        } return false;
    }
    uint64_t allocate_id() { while(next_record_id) { uint64_t id=next_record_id++; if(std::none_of(address_records.begin(),address_records.end(),[&](const auto& r){return r.stable_id==id;}))return id; } throw std::bad_alloc(); }
    bool cancelled(uint64_t epoch) const { return stopped.load() || cancellation.load() != epoch; }
    int stopped_error() { return fail(CE_NOT_RUNNING, L"Core has stopped."); }
    int cancelled_error() { return fail(CE_CANCELLED, L"Scan cancelled; previous committed results are unchanged."); }
};

Core::Core() : impl_(std::make_unique<Impl>()) {}
Core::~Core() { shutdown(); }
int Core::first_scan(const CeScanRequest& request) { return guarded([&] {
    if(impl_->stopped.load())return impl_->stopped_error();
    { std::unique_lock admission(impl_->scan_mutex,std::try_to_lock); if(!admission.owns_lock())return fail(CE_BUSY,L"Another scan is already running.");
      if(request.size!=sizeof(request)||request.reserved||(request.alignment!=1&&request.alignment!=4))return fail(CE_INVALID_ARGUMENT,L"V1 requires alignment 1 or 4 and the V1 request structure."); }
    const auto text=std::to_wstring(request.value);CeScanRequestV2 r{sizeof(r),CE_V2_VERSION,CE_TYPE_U32,CE_VALUE_SIGNED,CE_CMP_EXACT,request.alignment,4,0,request.begin,request.end,text.c_str(),nullptr};
    return impl_->typed_scan(r,false,true);
}); }
int Core::next_scan(int32_t value) { return guarded([&] { const auto text=std::to_wstring(value);CeScanRequestV2 r{sizeof(r),CE_V2_VERSION,CE_TYPE_U32,CE_VALUE_SIGNED,CE_CMP_EXACT,0,4,0,0,0,text.c_str(),nullptr};return impl_->typed_scan(r,true,true); }); }
void Core::cancel_scan() noexcept { impl_->cancellation.fetch_add(1); }
int Core::result_count(uint64_t& count) {
    return guarded([&] {
        std::lock_guard lock(impl_->state_mutex);
        if (impl_->stopped.load()) return impl_->stopped_error();
        if(!impl_->compatible_scan())return fail(CE_UNSUPPORTED,L"V1 result access cannot interpret the active V2 type.");
        count = impl_->active ? impl_->active->info.count : 0;
        return static_cast<int>(CE_OK);
    });
}
int Core::result(uint64_t index, CeResult& value) {
    return guarded([&] {
        std::lock_guard lock(impl_->state_mutex);
        if (impl_->stopped.load()) return impl_->stopped_error();
        if(!impl_->compatible_scan())return fail(CE_UNSUPPORTED,L"V1 result access cannot interpret the active V2 type.");
        if(!impl_->active)return fail(CE_INVALID_ARGUMENT,L"No active scan.");
        CeResult temporary{};std::vector<uint8_t> bytes;
        int status=impl_->active->result(index,temporary.address,bytes);if(status!=CE_OK)return status;
        std::memcpy(&temporary.value,bytes.data(),4);value=temporary;
        return static_cast<int>(CE_OK);
    });
}
int Core::read(uint64_t address, int32_t& value) {
    return guarded([&] {
        std::lock_guard lock(impl_->state_mutex);
        if (impl_->stopped.load()) return impl_->stopped_error();
        if (!valid_address(address)) return fail(CE_INVALID_ARGUMENT, L"Invalid or overflowing int32 address.");
        return read_value(address, value) ? static_cast<int>(CE_OK) : fail(CE_ACCESS_ERROR, L"Cannot read four bytes from this host address.");
    });
}
int Core::write(uint64_t address, int32_t value) {
    return guarded([&] {
        std::lock_guard lock(impl_->state_mutex);
        if (impl_->stopped.load()) return impl_->stopped_error();
        if (!valid_address(address)) return fail(CE_INVALID_ARGUMENT, L"Invalid or overflowing int32 address.");
        auto& records = impl_->address_records;
        auto found = std::find_if(records.begin(), records.end(), [&](const auto& item) { return item.legacy() && item.record.address == address; });
        const bool adding = found == records.end();
        if(adding&&records.size()>=max_table_records)return fail(CE_UNSUPPORTED,L"Record limit is 10000.");
        // Reserve before modifying host memory, so allocation failure cannot leave
        // a successful memory write without its corresponding address record.
        if (adding && records.size() == records.capacity()) records.reserve(records.size() + 1);
        if (!write_value(address, value)) return fail(CE_ACCESS_ERROR, L"Cannot write four bytes to a writable host address; no record changed.");
        if (adding) { StoredRecord item; item.stable_id=impl_->allocate_id();item.record = {address, value, false}; records.push_back(std::move(item)); }
        else {found->record.value = value;found->raw.clear();found->raw_valid=true;found->last_status=CE_OK;} ++impl_->revision;
        return static_cast<int>(CE_OK);
    });
}
int Core::freeze(uint64_t address, int32_t value, bool enabled) {
    return guarded([&] {
        std::lock_guard lock(impl_->state_mutex);
        if (impl_->stopped.load()) return impl_->stopped_error();
        if (!valid_address(address)) return fail(CE_INVALID_ARGUMENT, L"Invalid or overflowing int32 address.");
        auto& records = impl_->address_records;
        auto found = std::find_if(records.begin(), records.end(), [&](const auto& item) { return item.legacy() && item.record.address == address; });
        if (!enabled) {
            if (found != records.end()) {found->record.frozen = false;++impl_->revision;}
            return static_cast<int>(CE_OK);
        }
        if (!accessible(address, true)) return fail(CE_ACCESS_ERROR, L"Freeze requires a currently writable four-byte host address.");
        if(impl_->conflict(address,4,found==records.end()?0:found->stable_id))return fail(CE_INVALID_ARGUMENT,L"Enabled freeze byte ranges cannot overlap.");
        if (found == records.end()) {
            if(records.size()>=max_table_records)return fail(CE_UNSUPPORTED,L"Record limit is 10000.");
            StoredRecord item;
            item.stable_id=impl_->allocate_id(); item.record = {address, value, true};
            records.push_back(std::move(item));
        } else { found->record.value = value; found->record.frozen = true;found->raw.clear();found->raw_valid=true;found->last_status=CE_OK; }
        ++impl_->revision;
        // Scheduling does not write now: the runtime's freeze thread writes every 80 ms.
        return static_cast<int>(CE_OK);
    });
}
void Core::tick_freezes() {
    std::lock_guard lock(impl_->state_mutex);
    if (impl_->stopped.load()) return;
    for (auto& item : impl_->address_records) {
        if(!item.record.frozen)continue;uint64_t address=0;
        int status=item.raw_valid?typed::resolve(item.location(),address):CE_ACCESS_ERROR;
        if(status==CE_OK&&impl_->conflict(address,item.width,item.stable_id))status=CE_INVALID_ARGUMENT;
        if(status==CE_OK&&!typed::write(address,item.data(),item.width))status=CE_ACCESS_ERROR;
        item.last_status=status;if(status!=CE_OK){item.record.frozen=false;++impl_->revision;}else item.record.address=address;
    }
}
std::vector<AddressRecord> Core::records() {
    std::lock_guard lock(impl_->state_mutex);
    std::vector<AddressRecord> result;
    if (impl_->stopped.load()) return result;
    result.reserve(impl_->address_records.size());
    for (const auto& item : impl_->address_records) if(item.legacy())result.push_back(item.record);
    return result;
}
int Core::save_table(const wchar_t* path) {
    return guarded([&] {
        if (!path || !*path || std::wcslen(path) > 32700) return fail(CE_INVALID_ARGUMENT, L"Invalid CT destination path.");
        std::vector<StoredRecord> records;
        {
            std::lock_guard lock(impl_->state_mutex);
            if (impl_->stopped.load()) return impl_->stopped_error();
            if (impl_->address_records.size() > max_table_records) return fail(CE_UNSUPPORTED, L"CT supports at most 10000 records.");
            std::set<uint64_t> addresses;
            for (const auto& record : impl_->address_records) {
                if (!record.legacy())
                    return fail(CE_UNSUPPORTED, L"V1 CT cannot represent typed records; use V2 export.");
                if (!addresses.insert(record.record.address).second)
                    return fail(CE_UNSUPPORTED, L"V1 CT cannot represent duplicate addresses; use V2 export.");
            }
            records = impl_->address_records;
        }
        ComScope com;
        if (!com.valid()) return fail(CE_INTERNAL_ERROR, L"Cannot initialize XML services.");
        ComPointer<IStream> stream;
        if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.put()))) return fail(CE_INTERNAL_ERROR, L"Cannot allocate CT output stream.");
        auto status = serialize_table(records, stream.value);
        if (status != CE_OK) return status;
        STATSTG info{};
        if (FAILED(stream->Stat(&info, STATFLAG_NONAME))) return fail(CE_IO_ERROR, L"Cannot measure CT output.");
        if (info.cbSize.QuadPart > max_table_bytes) return fail(CE_UNSUPPORTED, L"CT output exceeds the supported 8 MiB limit.");
        LARGE_INTEGER zero{};
        if (FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr))) return fail(CE_IO_ERROR, L"Cannot seek CT output.");
        TemporaryFile temporary;
        status = create_temporary(std::wstring(path) + L".ce-save-", temporary, false);
        if (status != CE_OK) return status;
        std::array<unsigned char, 64 * 1024> buffer{};
        uint64_t remaining = info.cbSize.QuadPart;
        while (remaining) {
            const auto requested = static_cast<ULONG>((std::min)(remaining, static_cast<uint64_t>(buffer.size())));
            ULONG received = 0;
            if (FAILED(stream->Read(buffer.data(), requested, &received)) || received != requested ||
                !write_exact(temporary.handle.value, buffer.data(), received)) return fail(CE_IO_ERROR, L"Cannot write the CT transaction.");
            remaining -= received;
        }
        if (!FlushFileBuffers(temporary.handle.value)) return win_fail(L"Cannot flush the CT transaction");
        temporary.handle.close();
        std::lock_guard lock(impl_->state_mutex);
        if (impl_->stopped.load()) return impl_->stopped_error();
        if (!MoveFileExW(temporary.path.c_str(), path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return win_fail(L"Cannot atomically replace the CT destination");
        temporary.path.clear();
        return static_cast<int>(CE_OK);
    });
}
int Core::load_table(const wchar_t* path) {
    return guarded([&] {
        if (impl_->stopped.load()) return impl_->stopped_error();
        uint64_t revision;{std::lock_guard lock(impl_->state_mutex);revision=impl_->revision;}
        if (!path || !*path || std::wcslen(path) > 32700) return fail(CE_INVALID_ARGUMENT, L"Invalid CT input path.");
        Handle file(CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!file.valid()) return win_fail(L"Cannot open CT input");
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file.value, &size)) return win_fail(L"Cannot measure CT input");
        if (size.QuadPart <= 0 || static_cast<uint64_t>(size.QuadPart) > max_table_bytes)
            return fail(CE_UNSUPPORTED, L"CT input must be nonempty and no larger than 8 MiB.");
        std::vector<unsigned char> bytes(static_cast<size_t>(size.QuadPart));
        if (!read_exact(file.value, bytes.data(), static_cast<DWORD>(bytes.size()))) return win_fail(L"Cannot read CT input");
        std::vector<StoredRecord> records;
        TableReader reader;
        const auto status = reader.parse(bytes, records);
        if (status != CE_OK) return status;
        std::lock_guard lock(impl_->state_mutex);
        if (impl_->stopped.load()) return impl_->stopped_error();
        if(impl_->revision!=revision)return fail(CE_BUSY,L"Records changed during CT import; previous records retained.");
        std::set<uint64_t> ids;for(auto& item:records)if(item.has_id){uint64_t id=0;parse_unsigned(item.id,10,id);if(id){item.stable_id=id;ids.insert(id);}}
        uint64_t candidate=impl_->next_record_id;for(auto& item:records)if(!item.stable_id){while(candidate&&ids.count(candidate))++candidate;if(!candidate)return fail(CE_INTERNAL_ERROR,L"Record IDs exhausted.");item.stable_id=candidate;ids.insert(candidate++);}impl_->next_record_id=candidate;
        impl_->address_records.swap(records);++impl_->revision;
        return static_cast<int>(CE_OK);
    });
}
#include "typed_core.inc"
#include "typed_table.inc"
#include "process_view.inc"
#include "pointer_scan.inc"

void Core::request_stop() noexcept {
    impl_->stopped.store(true);
    cancel_scan();
}
void Core::shutdown() {
    request_stop();
    // Runtime also drains its shared API lock before destruction. Taking this
    // mutex makes explicit shutdown safe even when used directly by core tests.
    std::lock_guard scan_lock(impl_->scan_mutex);
    std::lock_guard state_lock(impl_->state_mutex);
    impl_->address_records.clear();
    impl_->active.reset();impl_->undo.reset();
}
} // namespace ce
