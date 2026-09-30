namespace {
// Read-only views of the hosting process. Nothing here mutates host state, and the
// thread walk asks only for THREAD_QUERY_LIMITED_INFORMATION, so suspending or
// terminating a thread is impossible by access mask rather than by discipline.
//
// These enumerations deliberately do not take state_mutex: they read no Core state,
// and holding it for a full region walk would stall the 80 ms freeze tick.
int walk_regions(std::vector<CeRegionInfoV2>& out){
    SYSTEM_INFO system{};GetSystemInfo(&system);
    const uint64_t low=reinterpret_cast<uintptr_t>(system.lpMinimumApplicationAddress);
    const uint64_t high=uint64_t(reinterpret_cast<uintptr_t>(system.lpMaximumApplicationAddress))+1;
    out.clear();uint64_t cursor=low;
    while(cursor<high){MEMORY_BASIC_INFORMATION memory{};
        if(!VirtualQuery(reinterpret_cast<void*>(uintptr_t(cursor)),&memory,sizeof(memory)))return fail(CE_ACCESS_ERROR,L"Cannot enumerate host memory regions.");
        const uint64_t next=reinterpret_cast<uintptr_t>(memory.BaseAddress)+memory.RegionSize;
        if(next<=cursor)return fail(CE_ACCESS_ERROR,L"Invalid memory region during enumeration.");
        if(out.size()>=CE_V2_MAX_VIEW_ITEMS)return fail(CE_UNSUPPORTED,L"Host has more memory regions than this build enumerates.");
        CeRegionInfoV2 info{sizeof(info),CE_V2_VERSION};
        info.state=memory.State;info.type=memory.Type;info.allocation_protect=memory.AllocationProtect;info.protect=memory.Protect;
        info.base=reinterpret_cast<uintptr_t>(memory.BaseAddress);info.region_size=uint64_t(memory.RegionSize);
        out.push_back(info);cursor=(std::min)(next,high);}
    return CE_OK;
}
// A concurrent LoadLibrary in the host can leave the loader snapshot short; the
// documented remedy is to take it again.
bool open_snapshot(DWORD flags,Handle& snapshot,const wchar_t* message){
    for(int attempt=0;attempt<4;++attempt){snapshot.close();snapshot.value=CreateToolhelp32Snapshot(flags,GetCurrentProcessId());
        if(snapshot.valid())return true;
        const DWORD code=GetLastError();
        if(code!=ERROR_BAD_LENGTH&&code!=ERROR_PARTIAL_COPY){win_fail(message);return false;}}
    win_fail(message);return false;
}
int walk_modules(std::vector<CeModuleInfoV2>& out){
    out.clear();Handle snapshot;if(!open_snapshot(TH32CS_SNAPMODULE,snapshot,L"Cannot snapshot host modules"))return CE_IO_ERROR;
    MODULEENTRY32W entry{};entry.dwSize=sizeof(entry);
    if(!Module32FirstW(snapshot.value,&entry))return win_fail(L"Cannot enumerate host modules");
    do{if(out.size()>=CE_V2_MAX_VIEW_ITEMS)return fail(CE_UNSUPPORTED,L"Host has more modules than this build enumerates.");
        CeModuleInfoV2 info{sizeof(info),CE_V2_VERSION};
        info.base=reinterpret_cast<uintptr_t>(entry.modBaseAddr);info.module_size=uint64_t(entry.modBaseSize);
        lstrcpynW(info.path,entry.szExePath,static_cast<int>(std::size(info.path)));
        out.push_back(info);entry.dwSize=sizeof(entry);}while(Module32NextW(snapshot.value,&entry));
    return CE_OK;
}
int walk_threads(std::vector<CeThreadInfoV2>& out){
    out.clear();const DWORD process_id=GetCurrentProcessId(),current_id=GetCurrentThreadId();
    Handle snapshot;if(!open_snapshot(TH32CS_SNAPTHREAD,snapshot,L"Cannot snapshot host threads"))return CE_IO_ERROR;
    THREADENTRY32 entry{};entry.dwSize=sizeof(entry);
    if(!Thread32First(snapshot.value,&entry))return win_fail(L"Cannot enumerate host threads");
    do{if(entry.th32OwnerProcessID!=process_id)continue;
        if(out.size()>=CE_V2_MAX_VIEW_ITEMS)return fail(CE_UNSUPPORTED,L"Host has more threads than this build enumerates.");
        const DWORD thread_id=entry.th32ThreadID;const bool self=thread_id==current_id;
        CeThreadInfoV2 info{sizeof(info),CE_V2_VERSION};
        info.thread_id=thread_id;info.current=self?1u:0u;
        Handle opened(self?INVALID_HANDLE_VALUE:OpenThread(THREAD_QUERY_LIMITED_INFORMATION,FALSE,thread_id));
        const HANDLE handle=self?GetCurrentThread():opened.value;
        // A thread that exited between the snapshot and the open, or one we may not
        // query, clears the affected bits instead of failing the whole enumeration.
        if(handle){
            const int priority=GetThreadPriority(handle);
            if(priority!=THREAD_PRIORITY_ERROR_RETURN){info.priority=uint32_t(int32_t(priority));info.valid_mask|=1u<<1;}
            FILETIME creation{},exit_time{},kernel{},user{};
            if(GetThreadTimes(handle,&creation,&exit_time,&kernel,&user)){info.created=(uint64_t(creation.dwHighDateTime)<<32)|creation.dwLowDateTime;info.valid_mask|=1u<<2;}
            wchar_t* description=nullptr;
            if(SUCCEEDED(GetThreadDescription(handle,&description))&&description){lstrcpynW(info.description,description,static_cast<int>(std::size(info.description)));if(info.description[0])info.valid_mask|=1u;LocalFree(description);}}
        out.push_back(info);entry.dwSize=sizeof(entry);}while(Thread32Next(snapshot.value,&entry));
    return CE_OK;
}
}
int Core::regions_v2(std::vector<CeRegionInfoV2>& out){return guarded([&]{if(impl_->stopped.load())return impl_->stopped_error();return walk_regions(out);});}
int Core::modules_v2(std::vector<CeModuleInfoV2>& out){return guarded([&]{if(impl_->stopped.load())return impl_->stopped_error();return walk_modules(out);});}
int Core::threads_v2(std::vector<CeThreadInfoV2>& out){return guarded([&]{if(impl_->stopped.load())return impl_->stopped_error();return walk_threads(out);});}
