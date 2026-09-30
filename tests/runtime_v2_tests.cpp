#include <windows.h>
#include "ce/api_v2.h"
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <stdexcept>
#include <string>
#include <vector>

void check(bool b,const char* message){if(!b)throw std::runtime_error(message);std::printf("PASS %s\n",message);}
template<class T>T load(HMODULE m,const char* n){auto p=GetProcAddress(m,n);if(!p)throw std::runtime_error(std::string("export ")+n);return reinterpret_cast<T>(p);}
BOOL CALLBACK locate(HWND w,LPARAM p){DWORD pid=0;GetWindowThreadProcessId(w,&pid);if(pid==GetCurrentProcessId()&&GetPropW(w,L"SHADOWTRAINER_WINDOW")){*reinterpret_cast<HWND*>(p)=w;return FALSE;}return TRUE;}
// A live host keeps changing between the size query and the fill, so the count has to
// be re-read on every retry; that is exactly what the UI's load_view does.
template<class T,class Fetch>
bool fill_view(std::vector<T>& items,uint32_t required,Fetch fetch){
 for(int attempt=0;attempt<4;++attempt){
  items.assign(required,T{});
  const int status=fetch(items.data(),required,&required);
  if(status==CE_OK){items.resize(required);return true;}
  if(status!=CE_INVALID_ARGUMENT)return false;}
 return false;
}
void visibility(HWND w,bool visible){for(int i=0;i<100&&bool(IsWindowVisible(w))!=visible;++i)Sleep(10);check(bool(IsWindowVisible(w))==visible,visible?"menu visible":"menu hidden");}
int wmain(int argc,wchar_t** argv){
 if(argc!=2)return 2;
 HMODULE module=nullptr;
 decltype(&CE_RequestStop) stop=nullptr; decltype(&CE_WaitStopped) wait=nullptr;
 try{
  module=LoadLibraryW(argv[1]);check(module!=nullptr,"load stage2 DLL");
#define FN(name) auto name=load<decltype(&::name)>(module,#name)
  FN(CE_GetState);FN(CE_GetApiVersion);FN(CE_GetApiVersionV2);
  stop=load<decltype(stop)>(module,"CE_RequestStop");wait=load<decltype(wait)>(module,"CE_WaitStopped");
  FN(CE_FirstScanV2);FN(CE_NextScanV2);FN(CE_GetScanInfoV2);FN(CE_GetResultV2);FN(CE_NewScanV2);FN(CE_UndoScanV2);
  FN(CE_ReadBytesV2);FN(CE_WriteBytesV2);FN(CE_WriteValueV2);FN(CE_FormatValueV2);FN(CE_ResolvePointerV2);FN(CE_ResolveAddressV2);
  FN(CE_UpsertRecordV2);FN(CE_RecordCountV2);FN(CE_GetRecordV2);FN(CE_RemoveRecordV2);FN(CE_WriteRecordV2);FN(CE_FreezeRecordV2);
  FN(CE_SaveTableV2);FN(CE_LoadTableV2);FN(CE_ToggleWindowV2);FN(CE_ShowWindow);FN(CE_FirstScan);FN(CE_GetResult);
  FN(CE_GetRegionsV2);FN(CE_GetModulesV2);FN(CE_GetThreadsV2);
#undef FN
  for(int i=0;i<400&&CE_GetState()==CE_STARTING;++i)Sleep(10);
  check(CE_GetState()==CE_RUNNING,"automatic v2 runtime ready");
  check(CE_GetApiVersion()==1&&CE_GetApiVersionV2()==2,"v1 and v2 versions preserved");
  HWND window=nullptr;EnumWindows(locate,reinterpret_cast<LPARAM>(&window));check(window!=nullptr,"stable menu window property");visibility(window,true);
  check(CE_ToggleWindowV2()==CE_OK,"toggle dispatch");visibility(window,false);
  check(CE_ToggleWindowV2()==CE_OK,"toggle hidden window dispatch");visibility(window,true);
  check(CE_ShowWindow()==CE_OK,"v1 show remains show-only");visibility(window,true);
  alignas(8) uint64_t target=UINT64_MAX-1; const auto address=reinterpret_cast<uintptr_t>(&target);
  wchar_t address_text[32]{};swprintf_s(address_text,L"%llX",static_cast<unsigned long long>(address));uint64_t parsed_address=0;
  check(CE_ResolveAddressV2(address_text,&parsed_address)==CE_OK&&parsed_address==address,"absolute address expression resolution");
  CeScanRequestV2 r{sizeof(r),2,CE_TYPE_U64,0,CE_CMP_EXACT,8,0,0,address,address+8,L"18446744073709551614",nullptr};
  auto invalid=r;invalid.version=9;check(CE_FirstScanV2(&invalid)==CE_INVALID_ARGUMENT,"reject v2 version mismatch");
  auto bad_rounding=r;bad_rounding.rounding=4;check(CE_FirstScanV2(&bad_rounding)==CE_INVALID_ARGUMENT,"reject unknown rounding mode at the ABI boundary");
  auto wrong_type=r;wrong_type.rounding=CE_ROUND_ROUNDED;check(CE_FirstScanV2(&wrong_type)==CE_INVALID_ARGUMENT,"reject rounding on an integer scan");
  check(CE_FirstScanV2(&r)==CE_OK,"uint64 exact avoids double rounding");
  CeScanInfoV2 info{};info.size=sizeof(info);info.version=2;check(CE_GetScanInfoV2(&info)==CE_OK&&info.count==1&&info.byte_length==8,"typed scan metadata");
  const auto previous_generation=info.generation;
  CeResultV2 result{};result.size=sizeof(result);result.version=2;uint32_t required=0;
  check(CE_GetResultV2(info.generation,0,&result,nullptr,0,&required)==CE_OK&&required==8,"caller-owned result size query");
  uint64_t observed=0;check(CE_GetResultV2(info.generation,0,&result,&observed,8,&required)==CE_OK&&observed==target,"raw uint64 result");
  CeResult legacy{};check(CE_GetResult(0,&legacy)==CE_UNSUPPORTED,"legacy result refuses typed truncation");
  wchar_t formatted[128]{};check(CE_FormatValueV2(CE_TYPE_U64,0,&observed,8,formatted,128,&required)==CE_OK&&wcscmp(formatted,L"18446744073709551614")==0,"uint64 exact formatting");
  target-=1;r.comparison=CE_CMP_DECREASED;r.value=nullptr;check(CE_NextScanV2(&r)==CE_OK,"relative typed next scan");
  check(CE_GetResultV2(previous_generation,0,&result,&observed,8,&required)==CE_BUSY,"reject stale result generation");
  check(CE_UndoScanV2()==CE_OK,"undo restores prior committed scan");
  check(CE_GetScanInfoV2(&info)==CE_OK&&CE_GetResultV2(info.generation,0,&result,&observed,8,&required)==CE_OK&&observed==UINT64_MAX-1,"undo retains historical raw value");
  check(CE_WriteValueV2(address,CE_TYPE_U64,0,8,L"42")==CE_OK&&target==42,"typed memory write");
  CeRecordRequestV2 record{};record.size=sizeof(record);record.version=2;record.type=CE_TYPE_U64;record.byte_length=8;record.address.base=address;record.description=L"wide integer";record.value=L"99";
  uint64_t id=0;check(CE_UpsertRecordV2(&record,&id)==CE_OK&&id&&target==42,"record adds without memory write");
  check(CE_WriteRecordV2(id,L"99")==CE_OK&&target==99,"record writes typed value");
  check(CE_FreezeRecordV2(id,1)==CE_OK,"typed freeze enabled");target=12;Sleep(250);check(target==99,"typed freeze applied");
  check(CE_FreezeRecordV2(id,0)==CE_OK,"typed freeze disabled");
  CeRecordInfoV2 ri{};ri.size=sizeof(ri);ri.version=2;uint64_t count=0;
  check(CE_RecordCountV2(&count)==CE_OK&&count>=1,"address list count");
  bool found=false;for(uint64_t i=0;i<count;++i){check(CE_GetRecordV2(i,&ri,formatted,128,&required)==CE_OK,"address record metadata");if(ri.id==id)found=true;}check(found,"stable record ID enumerated");
  uintptr_t pointer=address;CeAddressV2 chain{};chain.base=reinterpret_cast<uintptr_t>(&pointer);chain.offset_count=1;chain.offsets[0]=0;uint64_t resolved=0;
  check(CE_ResolvePointerV2(&chain,&resolved)==CE_OK&&resolved==address,"pointer chain resolves host pointer width");
  uint64_t second=456;pointer=reinterpret_cast<uintptr_t>(&second);check(CE_ResolvePointerV2(&chain,&resolved)==CE_OK&&resolved==pointer,"pointer chain follows relocation");
  check(CE_ReadBytesV2(resolved,&observed,8)==CE_OK&&observed==456,"raw memory preview read");
  {
    // A raw write must place the bytes in address order with no parsing at all.
    alignas(8) uint8_t scratch[8]{};const uint8_t pattern[4]={0xDE,0xAD,0xBE,0xEF};
    const auto scratch_address=reinterpret_cast<uintptr_t>(scratch);
    check(CE_WriteBytesV2(scratch_address,pattern,4)==CE_OK,"raw byte write");
    check(std::memcmp(scratch,pattern,4)==0,"raw byte write stores the exact pattern in address order");
    check(CE_WriteBytesV2(scratch_address,nullptr,4)==CE_INVALID_ARGUMENT&&CE_WriteBytesV2(scratch_address,pattern,0)==CE_INVALID_ARGUMENT,"raw byte write rejects null and zero length");
    check(CE_WriteBytesV2(scratch_address,pattern,CE_V2_MAX_VALUE_BYTES+1)==CE_INVALID_ARGUMENT,"raw byte write rejects an oversized length");
    check(CE_WriteBytesV2(1,pattern,4)!=CE_OK,"raw byte write rejects an unmapped address");
  }
  wchar_t path[MAX_PATH]{},temp[MAX_PATH]{};GetTempPathW(MAX_PATH,temp);GetTempFileNameW(temp,L"cv2",0,path);
  check(CE_SaveTableV2(path)==CE_OK,"typed CT save");check(CE_RemoveRecordV2(id)==CE_OK,"remove address record");
  check(CE_LoadTableV2(path)==CE_OK,"typed CT load");DeleteFileW(path);target=123;Sleep(180);check(target==123,"CT import does not activate freeze");
  check(CE_NewScanV2()==CE_OK,"new scan resets session");
  alignas(8) double rounded_target=1.0000000000000002;const auto rounded_address=reinterpret_cast<uintptr_t>(&rounded_target);
  CeScanRequestV2 rounded{sizeof(rounded),2,CE_TYPE_DOUBLE,0,CE_CMP_EXACT,8,0,CE_ROUND_ROUNDED,rounded_address,rounded_address+8,L"1",nullptr};
  CeScanInfoV2 rounded_info{};rounded_info.size=sizeof(rounded_info);rounded_info.version=2;
  check(CE_FirstScanV2(&rounded)==CE_OK&&CE_GetScanInfoV2(&rounded_info)==CE_OK&&rounded_info.count==1,"rounded float scan matches a value exact comparison misses");
  rounded.rounding=CE_ROUND_EXACT;
  check(CE_FirstScanV2(&rounded)==CE_OK&&CE_GetScanInfoV2(&rounded_info)==CE_OK&&rounded_info.count==0,"exact comparison still misses the same value");
  check(CE_NewScanV2()==CE_OK,"new scan resets the rounding scan");
  check(sizeof(CeRegionInfoV2)==48&&sizeof(CeModuleInfoV2)==552&&sizeof(CeThreadInfoV2)==168,"process-view ABI sizes frozen");
  uint32_t regions_required=0,modules_required=0,threads_required=0;
  check(CE_GetRegionsV2(nullptr,0,&regions_required)==CE_OK&&regions_required>0,"region count size query");
  check(CE_GetModulesV2(nullptr,0,&modules_required)==CE_OK&&modules_required>0,"module count size query");
  check(CE_GetThreadsV2(nullptr,0,&threads_required)==CE_OK&&threads_required>0,"thread count size query");
  check(CE_GetRegionsV2(nullptr,4,&regions_required)==CE_INVALID_ARGUMENT,"null buffer with nonzero capacity is rejected");
  check(CE_GetRegionsV2(nullptr,0,nullptr)==CE_INVALID_ARGUMENT,"missing required pointer is rejected");
  {
    std::vector<CeRegionInfoV2> regions;
    check(fill_view(regions,regions_required,[&](CeRegionInfoV2* items,uint32_t capacity,uint32_t* required){return CE_GetRegionsV2(items,capacity,required);}),"region enumeration");
    SYSTEM_INFO system{};GetSystemInfo(&system);
    bool tiled=true,self_describing=true,covers_caller=false,committed=false,freed=false;
    for(size_t i=0;i<regions.size();++i){
      self_describing&=regions[i].size==sizeof(CeRegionInfoV2)&&regions[i].version==CE_V2_VERSION;
      const uint64_t base=regions[i].base,size=regions[i].region_size;
      tiled&=size>0&&(i?base==regions[i-1].base+regions[i-1].region_size:base==reinterpret_cast<uintptr_t>(system.lpMinimumApplicationAddress));
      const auto here=reinterpret_cast<uint64_t>(&regions_required);
      covers_caller|=here>=base&&here<base+size;
      committed|=regions[i].state==MEM_COMMIT;freed|=regions[i].state==MEM_FREE;}
    check(self_describing,"every region element self-describes");
    check(tiled,"regions tile the address space from the minimum application address");
    check(covers_caller,"the caller's own stack address falls inside a reported region");
    check(committed&&freed,"region states include both committed and free space");
    // The host keeps allocating while these run, so a snapshot is never compared
    // against a count taken earlier; only invariants that survive a live host.
    CeRegionInfoV2 single[2];std::memset(single,0xAB,sizeof(single));const auto untouched=single[0];uint32_t probe=0;
    check(CE_GetRegionsV2(single,1,&probe)==CE_INVALID_ARGUMENT&&probe>1,"short buffer is rejected and still reports the count");
    check(std::memcmp(&single[0],&untouched,sizeof(untouched))==0,"short buffer writes nothing at all");
    std::vector<CeRegionInfoV2> spare(regions.size()+64);
    check(CE_GetRegionsV2(spare.data(),static_cast<uint32_t>(spare.size()),&probe)==CE_OK&&probe<=spare.size(),"a buffer with headroom always fills");
  }
  {
    std::vector<CeModuleInfoV2> modules;
    check(fill_view(modules,modules_required,[&](CeModuleInfoV2* items,uint32_t capacity,uint32_t* required){return CE_GetModulesV2(items,capacity,required);}),"module enumeration");
    const auto main_module=reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
    bool has_main=false,self_describing=true,path_ok=true;
    for(const auto& m:modules){self_describing&=m.size==sizeof(CeModuleInfoV2)&&m.version==CE_V2_VERSION;
      if(m.base!=main_module)continue;has_main=true;
      const std::wstring module_path=m.path;path_ok=module_path.size()>4&&module_path.compare(module_path.size()-4,4,L".exe")==0;}
    check(self_describing,"every module element self-describes");
    check(has_main&&path_ok,"module list contains the hosting executable with an .exe path");
  }
  {
    std::vector<CeThreadInfoV2> threads;
    check(fill_view(threads,threads_required,[&](CeThreadInfoV2* items,uint32_t capacity,uint32_t* required){return CE_GetThreadsV2(items,capacity,required);}),"thread enumeration");
    unsigned current=0;bool self_describing=true,current_matches=true;
    for(const auto& t:threads){self_describing&=t.size==sizeof(CeThreadInfoV2)&&t.version==CE_V2_VERSION;
      if(t.current){++current;current_matches&=t.thread_id==GetCurrentThreadId();}}
    check(self_describing,"every thread element self-describes");
    check(current==1&&current_matches,"exactly one thread is flagged as the caller");
  }
  int32_t old=707;CeScanRequest old_request{sizeof(old_request),4,reinterpret_cast<uintptr_t>(&old),reinterpret_cast<uintptr_t>(&old)+4,707,0};
  check(CE_FirstScan(&old_request)==CE_OK&&CE_GetResult(0,&legacy)==CE_OK&&legacy.value==707,"v1 scan still works after v2");
  check(stop()==CE_OK&&wait(10000)==CE_OK,"stage2 shutdown and worker drain");
  check(FreeLibrary(module)!=0,"stage2 module reference released");module=nullptr;
  std::puts("STAGE2_RUNTIME_PASS typed scans/records/pointers/CT/menu; host alive");return 0;
 }catch(const std::exception& e){std::fprintf(stderr,"FAIL %s Win32=%lu\n",e.what(),GetLastError());if(module&&stop&&wait&&stop()==CE_OK&&wait(10000)==CE_OK)FreeLibrary(module);return 1;}
}
