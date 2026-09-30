#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "typed_scan.hpp"
#include "core.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
namespace ce::typed {
namespace {
std::atomic<uint64_t> serial{0};
struct Header {uint64_t magic=0x3253434546494c45ull;CeScanInfoV2 info{};};
int error(int c,const wchar_t* s){set_error(s);return c;}
bool seek(HANDLE f,uint64_t n){if(n>INT64_MAX)return false;LARGE_INTEGER p{};p.QuadPart=LONGLONG(n);return SetFilePointerEx(f,p,nullptr,FILE_BEGIN)!=0;}
bool io(HANDLE f,void* p,DWORD n,bool write){auto b=static_cast<uint8_t*>(p);while(n){DWORD done=0;BOOL ok=write?WriteFile(f,b,n,&done,nullptr):ReadFile(f,b,n,&done,nullptr);if(!ok||!done)return false;b+=done;n-=done;}return true;}
struct Buffer {uint8_t* p;size_t n;explicit Buffer(size_t size):p(static_cast<uint8_t*>(VirtualAlloc(nullptr,size,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE))),n(size){if(!p)throw std::bad_alloc();}~Buffer(){VirtualFree(p,0,MEM_RELEASE);}};
struct Writer {HANDLE file;Buffer data{65536};size_t used=0;bool flush(){if(!used)return true;if(!io(file,data.p,DWORD(used),true))return false;used=0;return true;}bool put(const void* p,size_t n){auto src=static_cast<const uint8_t*>(p);while(n){size_t chunk=(std::min)(n,data.n-used);std::memcpy(data.p+used,src,chunk);used+=chunk;src+=chunk;n-=chunk;if(used==data.n&&!flush())return false;}return true;}};
struct Reader {HANDLE file;Buffer data{65536};size_t position=0,used=0;bool get(void* p,size_t n){auto dst=static_cast<uint8_t*>(p);while(n){if(position==used){DWORD got=0;if(!ReadFile(file,data.p,DWORD(data.n),&got,nullptr)||!got)return false;position=0;used=got;}size_t chunk=(std::min)(n,used-position);std::memcpy(dst,data.p+position,chunk);position+=chunk;dst+=chunk;n-=chunk;}return true;}};
struct Range{uint64_t begin,end;};
bool overlap(uint64_t a,uint64_t n,Range r){return a<r.end&&r.begin<a+n;}
Range range(const void* p,size_t n){auto a=uint64_t(reinterpret_cast<uintptr_t>(p));return{a,a+n};}
}
struct ScanFile::Impl {HANDLE file=INVALID_HANDLE_VALUE;std::wstring path;std::mutex mutex;~Impl(){if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);if(!path.empty())DeleteFileW(path.c_str());}};
ScanFile::ScanFile():impl(std::make_unique<Impl>()){}
ScanFile::~ScanFile()=default;
int ScanFile::stamp(uint64_t generation){std::lock_guard lock(impl->mutex);auto next=info;next.generation=generation;Header h;h.info=next;if(!seek(impl->file,0)||!io(impl->file,&h,sizeof(h),true)||!FlushFileBuffers(impl->file))return error(CE_IO_ERROR,L"Cannot commit scan header.");info=next;return CE_OK;}
int ScanFile::result(uint64_t index,uint64_t& address,std::vector<uint8_t>& bytes){std::lock_guard lock(impl->mutex);if(index>=info.count)return error(CE_INVALID_ARGUMENT,L"Result index is out of range.");uint64_t stride=8+info.byte_length;if(index>(INT64_MAX-sizeof(Header))/stride)return error(CE_IO_ERROR,L"Result position overflow.");std::vector<uint8_t> tmp(info.byte_length);uint64_t a;if(!seek(impl->file,sizeof(Header)+index*stride)||!io(impl->file,&a,8,false)||!io(impl->file,tmp.data(),info.byte_length,false))return error(CE_IO_ERROR,L"Cannot read typed scan result.");address=a;bytes.swap(tmp);return CE_OK;}
int scan(const CeScanRequestV2& input, const ScanFile* previous,
    const std::function<bool()>& cancelled, std::unique_ptr<ScanFile>& output,
    const ProgressCallback& progress) {
    ScanProgress work;
    if (previous) {
        work.phase = CE_SCAN_READING;
        work.unit = CE_SCAN_UNIT_CANDIDATES;
        work.total_known = true;
        work.total = previous->info.count;
    }
    auto report = [&] { if (progress) progress(work); };
    report();
    auto r=input;if(previous&&r.alignment==0)r.alignment=previous->info.alignment;Value a,b;uint32_t width=0;int status=prepare(r,previous!=nullptr,previous?previous->info.byte_length:0,a,b,width);if(status)return status;
    if(previous&&(r.type!=previous->info.type||r.flags!=previous->info.flags||width!=previous->info.byte_length||r.alignment!=previous->info.alignment))return error(CE_INVALID_ARGUMENT,L"Next scan cannot change type, flags, width or alignment.");
    SYSTEM_INFO sys{};GetSystemInfo(&sys);bool broad=!r.begin&&!r.end;uint64_t low=reinterpret_cast<uintptr_t>(sys.lpMinimumApplicationAddress),high=uint64_t(reinterpret_cast<uintptr_t>(sys.lpMaximumApplicationAddress))+1;
    if(!previous&&!broad&&(r.begin<low||r.begin>=r.end||r.end>high))return error(CE_INVALID_ARGUMENT,L"Invalid first-scan range.");
    std::vector<Range> ranges;
    if (!previous) {
        uint64_t cursor = broad ? low : r.begin, end = broad ? high : r.end;
        while (cursor < end) {
            if (cancelled()) return CE_CANCELLED;
            MEMORY_BASIC_INFORMATION memory{};
            if (!VirtualQuery(reinterpret_cast<void*>(uintptr_t(cursor)), &memory, sizeof(memory)))
                return error(CE_ACCESS_ERROR, L"Cannot inspect scan range.");
            const uint64_t next = reinterpret_cast<uintptr_t>(memory.BaseAddress) + memory.RegionSize;
            if (next <= cursor) return error(CE_ACCESS_ERROR, L"Invalid memory region.");
            const DWORD protection = memory.Protect & 255;
            if (memory.State == MEM_COMMIT && !(memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                (protection == PAGE_READONLY || protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
                 protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY)) {
                const uint64_t range_end = (std::min)(next, end);
                ranges.push_back({cursor, range_end});
                work.total += range_end - cursor;
            }
            cursor = (std::min)(next, end);
        }
        work.total_known = true;
        work.phase = CE_SCAN_READING;
        report();
    }
    auto pending=std::make_unique<ScanFile>();wchar_t temp[MAX_PATH+1]{};DWORD len=GetTempPathW(MAX_PATH,temp);if(!len||len>=MAX_PATH)return error(CE_IO_ERROR,L"Cannot obtain scan temporary directory.");for(unsigned attempt=0;attempt<128;++attempt){pending->impl->path=std::wstring(temp)+L"ce-typed-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(serial.fetch_add(1))+L".tmp";pending->impl->file=CreateFileW(pending->impl->path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);if(pending->impl->file!=INVALID_HANDLE_VALUE)break;DWORD e=GetLastError();pending->impl->path.clear();if(e!=ERROR_FILE_EXISTS&&e!=ERROR_ALREADY_EXISTS)return error(CE_IO_ERROR,L"Cannot create scan transaction.");}if(pending->impl->file==INVALID_HANDLE_VALUE)return error(CE_IO_ERROR,L"Cannot create unique scan transaction.");
    pending->info={sizeof(CeScanInfoV2),CE_V2_VERSION,r.type,r.flags,width,r.alignment,0,0,0,0};Header header;header.info=pending->info;if(!io(pending->impl->file,&header,sizeof(header),true))return error(CE_IO_ERROR,L"Cannot initialize scan header.");Writer writer{pending->impl->file};
    auto append=[&](uint64_t address,const uint8_t* value){uint64_t stride=8+width;if(pending->info.count>=(INT64_MAX-sizeof(Header))/stride)return false;if(!writer.put(&address,8)||!writer.put(value,width))return false;++pending->info.count;return true;};
    if (previous) {
        HANDLE source = ReOpenFile(previous->impl->file, GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_FLAG_SEQUENTIAL_SCAN);
        if (source == INVALID_HANDLE_VALUE) return error(CE_IO_ERROR, L"Cannot reopen prior candidates.");
        struct Close { HANDLE file; ~Close() { CloseHandle(file); } } close{source};
        if (!seek(source, sizeof(Header))) return error(CE_IO_ERROR, L"Cannot seek prior scan candidates.");
        Reader reader{source};
        Buffer old(width), current(width);
        const uint64_t batch_size = (std::max)(uint64_t{1}, (std::min)(uint64_t{256}, uint64_t{65536} / (8 + width)));
        while (work.processed < work.total) {
            const uint64_t batch = (std::min)(batch_size, work.total - work.processed);
            for (uint64_t i = 0; i < batch; ++i) {
                if (cancelled()) return CE_CANCELLED;
                uint64_t address = 0;
                if (!reader.get(&address, 8) || !reader.get(old.p, width))
                    return error(CE_IO_ERROR, L"Cannot stream prior candidates.");
                if (read(address, current.p, width) &&
                    matches(r.comparison, r.type, r.flags, width, current.p, old.p, a, b, r.rounding) && !append(address, current.p))
                    return error(CE_IO_ERROR, L"Cannot stream next-scan output.");
            }
            work.processed += batch;
            report();
        }
    } else {
        Buffer buffer(size_t(sys.dwPageSize) + width - 1);
        std::vector<Range> excluded{range(buffer.p, buffer.n), range(writer.data.p, writer.data.n),
            range(ranges.data(), ranges.capacity() * sizeof(Range)), range(a.bytes.data(), a.bytes.capacity()),
            range(a.mask.data(), a.mask.capacity()), range(b.bytes.data(), b.bytes.capacity())};
        MEMORY_BASIC_INFORMATION stack{};
        if (VirtualQuery(&writer, &stack, sizeof(stack))) {
            uint64_t start = reinterpret_cast<uintptr_t>(stack.AllocationBase), end = start;
            MEMORY_BASIC_INFORMATION memory{};
            while (VirtualQuery(reinterpret_cast<void*>(uintptr_t(end)), &memory, sizeof(memory)) &&
                memory.AllocationBase == stack.AllocationBase) {
                const uint64_t next = reinterpret_cast<uintptr_t>(memory.BaseAddress) + memory.RegionSize;
                if (next <= end) break;
                end = next;
            }
            excluded.push_back({start, end});
        }
        size_t carry = 0;
        uint64_t previous_end = 0;
        for (const auto memory : ranges) {
            for (uint64_t cursor = memory.begin; cursor < memory.end;) {
                if (cancelled()) return CE_CANCELLED;
                if (cursor != previous_end) carry = 0;
                const SIZE_T wanted = SIZE_T((std::min)(memory.end - cursor,
                    uint64_t(sys.dwPageSize - cursor % sys.dwPageSize)));
                SIZE_T received = 0;
                ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(uintptr_t(cursor)),
                    buffer.p + carry, wanted, &received);
                received = (std::min)(received, wanted);
                const size_t total = carry + received;
                const uint64_t base = cursor - carry;
                size_t offset = size_t((r.alignment - base % r.alignment) % r.alignment);
                for (; received && offset + width <= total; offset += r.alignment) {
                    if ((offset & 4095) == 0 && cancelled()) return CE_CANCELLED;
                    const uint64_t address = base + offset;
                    bool skip = false;
                    if (broad) for (const auto exclusion : excluded) {
                        if (overlap(address, width, exclusion)) { skip = true; break; }
                    }
                    if (!skip && matches(r.comparison, r.type, r.flags, width, buffer.p + offset, nullptr, a, b, r.rounding) &&
                        !append(address, buffer.p + offset)) return error(CE_IO_ERROR, L"Cannot stream first-scan output.");
                }
                if (received) {
                    carry = (std::min)(size_t(width - 1), total);
                    std::memmove(buffer.p, buffer.p + total - carry, carry);
                    previous_end = cursor + received;
                } else {
                    carry = 0;
                    previous_end = 0;
                }
                cursor += wanted;
                // Attempted source bytes, excluding carry and unreadable regions.
                work.processed += wanted;
                report();
            }
        }
    }
    if (cancelled()) return CE_CANCELLED;
    work.phase = CE_SCAN_WRITING;
    report();
    if (!writer.flush() || !FlushFileBuffers(pending->impl->file))
        return error(CE_IO_ERROR, L"Cannot flush scan transaction.");
    output = std::move(pending);
    return CE_OK;
}
}
