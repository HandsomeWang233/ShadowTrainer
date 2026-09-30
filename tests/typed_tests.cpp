#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "core.hpp"
#include <atomic>
#include <cmath>
#include <locale.h>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>
#include <string>
#include <set>
#include <sstream>
namespace {
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(__FUNCTION__)+":"+std::to_string(__LINE__)+" " #x);}while(false)
struct Memory {uint8_t* p;size_t n;explicit Memory(size_t size):p(static_cast<uint8_t*>(VirtualAlloc(nullptr,size,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE))),n(size){CHECK(p);}~Memory(){VirtualFree(p,0,MEM_RELEASE);}uint64_t address(size_t offset=0){return reinterpret_cast<uintptr_t>(p+offset);}template<class T>void put(size_t offset,T value){std::memcpy(p+offset,&value,sizeof(value));}template<class T>T get(size_t offset=0){T v;std::memcpy(&v,p+offset,sizeof(v));return v;}};
CeScanRequestV2 request(Memory& m,uint32_t type,const wchar_t* value=nullptr,uint32_t mode=CE_CMP_EXACT,uint32_t flags=0,uint32_t alignment=1,uint32_t width=0){return{sizeof(CeScanRequestV2),2,type,flags,mode,alignment,width,0,m.address(),m.address()+m.n,value,nullptr};}
CeScanInfoV2 info(ce::Core& c){CeScanInfoV2 i{};CHECK(c.scan_info_v2(i)==CE_OK);return i;}
std::vector<uint8_t> result(ce::Core& c,uint64_t index=0){CeResultV2 r{};std::vector<uint8_t> b;CHECK(c.result_v2(info(c).generation,index,r,b)==CE_OK);return b;}
uint64_t result_address(ce::Core& c,uint64_t index=0){CeResultV2 r{};std::vector<uint8_t> b;CHECK(c.result_v2(info(c).generation,index,r,b)==CE_OK);return r.address;}
std::wstring formatted(ce::Core& c,uint32_t t,uint32_t f,const void* p,uint32_t n){std::wstring s;CHECK(c.format_value_v2(t,f,p,n,s)==CE_OK);return s;}
struct File{std::wstring path;File(){wchar_t tmp[MAX_PATH],name[MAX_PATH];CHECK(GetTempPathW(MAX_PATH,tmp));CHECK(GetTempFileNameW(tmp,L"ct2",0,name));path=name;}~File(){DeleteFileW(path.c_str());}void put(const std::string& s){HANDLE f=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr);CHECK(f!=INVALID_HANDLE_VALUE);DWORD n=0;BOOL ok=WriteFile(f,s.data(),DWORD(s.size()),&n,nullptr);CloseHandle(f);CHECK(ok&&n==s.size());}std::string get(){HANDLE f=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);CHECK(f!=INVALID_HANDLE_VALUE);LARGE_INTEGER size{};CHECK(GetFileSizeEx(f,&size));std::string data(size_t(size.QuadPart),'\0');DWORD n=0;BOOL ok=ReadFile(f,data.data(),DWORD(data.size()),&n,nullptr);CloseHandle(f);CHECK(ok&&n==data.size());return data;}};
void test_integer_limits(){ce::Core c;Memory m(64);CHECK(c.write_value_v2(m.address(),CE_TYPE_U64,0,0,L"18446744073709551615")==CE_OK);CHECK(m.get<uint64_t>()==UINT64_MAX);CHECK(formatted(c,CE_TYPE_U64,0,m.p,8)==L"18446744073709551615");CHECK(c.write_value_v2(m.address(),CE_TYPE_U64,0,0,L"18446744073709551616")==CE_INVALID_ARGUMENT);CHECK(c.write_value_v2(m.address(),CE_TYPE_U64,CE_VALUE_SIGNED,0,L"-9223372036854775808")==CE_OK);CHECK(m.get<int64_t>()==INT64_MIN);CHECK(formatted(c,CE_TYPE_U64,CE_VALUE_SIGNED,m.p,8)==L"-9223372036854775808");CHECK(c.write_value_v2(m.address(),CE_TYPE_U64,CE_VALUE_SIGNED,8,L"9223372036854775808")==CE_INVALID_ARGUMENT);CHECK(c.write_value_v2(m.address(),CE_TYPE_U8,0,0,L"256")==CE_INVALID_ARGUMENT);CHECK(c.write_value_v2(m.address(),CE_TYPE_U8,CE_VALUE_SIGNED,0,L"-129")==CE_INVALID_ARGUMENT);CHECK(c.write_value_v2(m.address(),CE_TYPE_U8,CE_VALUE_SIGNED|CE_VALUE_HEX,0,L"FF")==CE_OK&&m.p[0]==255);CHECK(formatted(c,CE_TYPE_U8,CE_VALUE_SIGNED,m.p,1)==L"-1");CHECK(c.write_value_v2(m.address(),CE_TYPE_U16,0,1,L"1")==CE_INVALID_ARGUMENT);CHECK(c.write_value_v2(m.address(),CE_TYPE_FLOAT,CE_VALUE_SIGNED,0,L"1")==CE_INVALID_ARGUMENT);}
void test_modes_and_history(){ce::Core c;Memory m(32);m.put<uint64_t>(0,UINT64_MAX);m.put<uint64_t>(8,UINT64_MAX-1);m.put<uint64_t>(16,0);m.put<uint64_t>(24,4);auto r=request(m,CE_TYPE_U64,L"18446744073709551615",CE_CMP_EXACT,0,8);CHECK(c.first_scan_v2(r)==CE_OK&&info(c).count==1);auto generation=info(c).generation;m.put<uint64_t>(0,UINT64_MAX-1);r.comparison=CE_CMP_DECREASED_BY;r.value=L"1";r.byte_length=0;r.alignment=0;CHECK(c.next_scan_v2(r)==CE_OK&&info(c).count==1);CeResultV2 stale{};std::vector<uint8_t> bytes;CHECK(c.result_v2(generation,0,stale,bytes)==CE_BUSY);CHECK(c.undo_scan_v2()==CE_OK);bytes=result(c);uint64_t v;std::memcpy(&v,bytes.data(),8);CHECK(v==UINT64_MAX);CHECK(c.undo_scan_v2()==CE_INVALID_ARGUMENT);r.alignment=8;r.comparison=CE_CMP_UNKNOWN;r.value=nullptr;CHECK(c.first_scan_v2(r)==CE_OK&&info(c).count==4);r.comparison=CE_CMP_BETWEEN;r.value=L"0";r.value2=L"4";CHECK(c.next_scan_v2(r)==CE_OK&&info(c).count==2);r.comparison=CE_CMP_GREATER;r.value=L"0";CHECK(c.next_scan_v2(r)==CE_OK&&info(c).count==1);r.comparison=CE_CMP_UNCHANGED;r.value=nullptr;CHECK(c.next_scan_v2(r)==CE_OK&&info(c).count==1);m.put<uint64_t>(24,5);r.comparison=CE_CMP_INCREASED;CHECK(c.next_scan_v2(r)==CE_OK&&info(c).count==1);r.type=CE_TYPE_U32;CHECK(c.next_scan_v2(r)==CE_INVALID_ARGUMENT&&info(c).type==CE_TYPE_U64);uint64_t count=99;CHECK(c.result_count(count)==CE_UNSUPPORTED&&count==99);CHECK(c.next_scan(0)==CE_UNSUPPORTED);CHECK(c.new_scan_v2()==CE_OK&&info(c).byte_length==0&&info(c).count==0);CHECK(c.undo_scan_v2()==CE_INVALID_ARGUMENT);}
void test_integer_delta_overflow(){ce::Core c;Memory m(8);auto r=request(m,CE_TYPE_U64,nullptr,CE_CMP_UNKNOWN,0,8);m.put<uint64_t>(0,UINT64_MAX);CHECK(c.first_scan_v2(r)==CE_OK);m.put<uint64_t>(0,0);r.comparison=CE_CMP_INCREASED_BY;r.value=L"1";CHECK(c.next_scan_v2(r)==CE_OK&&info(c).count==0);m.put<int64_t>(0,INT64_MIN);r.flags=CE_VALUE_SIGNED;r.comparison=CE_CMP_UNKNOWN;r.value=nullptr;CHECK(c.first_scan_v2(r)==CE_OK);m.put<int64_t>(0,INT64_MAX);r.comparison=CE_CMP_DECREASED_BY;r.value=L"1";CHECK(c.next_scan_v2(r)==CE_OK&&info(c).count==0);m.put<int64_t>(0,0);r.comparison=CE_CMP_UNKNOWN;CHECK(c.first_scan_v2(r)==CE_OK);m.put<int64_t>(0,INT64_MIN);r.comparison=CE_CMP_INCREASED_BY;r.value=L"-9223372036854775808";CHECK(c.next_scan_v2(r)==CE_OK&&info(c).count==1);}
void test_float(){ce::Core c;Memory m(32);std::string original=setlocale(LC_NUMERIC,nullptr);if(setlocale(LC_NUMERIC,"German_Germany.1252")){CHECK(c.write_value_v2(m.address(),CE_TYPE_DOUBLE,0,8,L"1.5")==CE_OK&&m.get<double>()==1.5);CHECK(formatted(c,CE_TYPE_DOUBLE,0,m.p,8)==L"1.5");CHECK(c.write_value_v2(m.address(),CE_TYPE_DOUBLE,0,8,L"1,5")==CE_INVALID_ARGUMENT);}setlocale(LC_NUMERIC,original.c_str());m.put<double>(0,std::numeric_limits<double>::quiet_NaN());m.put<double>(8,std::numeric_limits<double>::infinity());m.put<double>(16,0.0);m.put<double>(24,-0.0);auto r=request(m,CE_TYPE_DOUBLE,L"nan",CE_CMP_EXACT,0,8);CHECK(c.first_scan_v2(r)==CE_OK&&info(c).count==0);r.value=L"inf";CHECK(c.first_scan_v2(r)==CE_OK&&info(c).count==1);r.value=L"0";CHECK(c.first_scan_v2(r)==CE_OK&&info(c).count==2);m.put<double>(16,-0.0);r.comparison=CE_CMP_CHANGED;r.value=nullptr;CHECK(c.next_scan_v2(r)==CE_OK&&info(c).count==1&&result_address(c)==m.address(16));r.comparison=CE_CMP_INCREASED_BY;r.value=L"inf";CHECK(c.next_scan_v2(r)==CE_INVALID_ARGUMENT);r.comparison=CE_CMP_UNKNOWN;r.value=nullptr;CHECK(c.first_scan_v2(r)==CE_OK&&info(c).count==4);r.comparison=CE_CMP_GREATER;r.value=L"-inf";CHECK(c.next_scan_v2(r)==CE_OK&&info(c).count==3);}
// CE's exact-float rounding modes. The bucket width comes from how many decimals the
// caller typed, so the same memory can match or not depending on the literal.
void test_float_rounding(){
    const double above_one=std::nextafter(1.0,2.0);
    {ce::Core c;Memory m(8);m.put<double>(0,above_one);auto r=request(m,CE_TYPE_DOUBLE,L"1",CE_CMP_EXACT,0,8);
        CHECK(c.first_scan_v2(r)==CE_OK&&info(c).count==0);
        r.rounding=CE_ROUND_ROUNDED;CHECK(c.first_scan_v2(r)==CE_OK&&info(c).count==1);
        r.rounding=CE_ROUND_EXTREME;CHECK(c.first_scan_v2(r)==CE_OK&&info(c).count==1);
        r.rounding=CE_ROUND_TRUNCATED;CHECK(c.first_scan_v2(r)==CE_OK&&info(c).count==1);}
    // Extreme rounding really is asymmetric in CE: the double form excludes the bound
    // while the single form includes it.
    {ce::Core c;Memory m(8);m.put<double>(0,2.0);auto r=request(m,CE_TYPE_DOUBLE,L"1",CE_CMP_EXACT,0,8);
        r.rounding=CE_ROUND_EXTREME;CHECK(c.first_scan_v2(r)==CE_OK&&info(c).count==0);}
    {ce::Core c;Memory m(4);m.put<float>(0,2.0f);auto r=request(m,CE_TYPE_FLOAT,L"1",CE_CMP_EXACT,0,4);
        r.rounding=CE_ROUND_EXTREME;CHECK(c.first_scan_v2(r)==CE_OK&&info(c).count==1);}
    // Three typed decimals give a 0.001 bucket.
    {ce::Core c;Memory m(8);m.put<double>(0,1.0006);auto r=request(m,CE_TYPE_DOUBLE,L"1.001",CE_CMP_EXACT,0,8);
        CHECK(c.first_scan_v2(r)==CE_OK&&info(c).count==0);
        r.rounding=CE_ROUND_ROUNDED;CHECK(c.first_scan_v2(r)==CE_OK&&info(c).count==1);
        r.rounding=CE_ROUND_TRUNCATED;CHECK(c.first_scan_v2(r)==CE_OK&&info(c).count==0);
        r.rounding=CE_ROUND_EXTREME;CHECK(c.first_scan_v2(r)==CE_OK&&info(c).count==1);}
    // Half-to-even, the way Delphi's RoundTo behaves.
    {ce::Core c;Memory m(8);m.put<double>(0,2.5);auto r=request(m,CE_TYPE_DOUBLE,L"2",CE_CMP_EXACT,0,8);
        r.rounding=CE_ROUND_ROUNDED;CHECK(c.first_scan_v2(r)==CE_OK&&info(c).count==1);
        r.value=L"3";CHECK(c.first_scan_v2(r)==CE_OK&&info(c).count==0);}
    {ce::Core c;Memory m(8);m.put<double>(0,3.5);auto r=request(m,CE_TYPE_DOUBLE,L"4",CE_CMP_EXACT,0,8);
        r.rounding=CE_ROUND_ROUNDED;CHECK(c.first_scan_v2(r)==CE_OK&&info(c).count==1);
        r.value=L"3";CHECK(c.first_scan_v2(r)==CE_OK&&info(c).count==0);}
    // An exponent in the literal forces accuracy 0, exactly as CE's floataccuracy does.
    {ce::Core c;Memory m(8);m.put<double>(0,1.4999);auto r=request(m,CE_TYPE_DOUBLE,L"1.5e0",CE_CMP_EXACT,0,8);
        r.rounding=CE_ROUND_ROUNDED;CHECK(c.first_scan_v2(r)==CE_OK&&info(c).count==0);
        r.value=L"1.5";CHECK(c.first_scan_v2(r)==CE_OK&&info(c).count==1);}
    // Rounding is not part of the next-scan immutability set, so it can be switched.
    {ce::Core c;Memory m(8);m.put<double>(0,above_one);auto r=request(m,CE_TYPE_DOUBLE,nullptr,CE_CMP_UNKNOWN,0,8);
        CHECK(c.first_scan_v2(r)==CE_OK&&info(c).count==1);
        r.comparison=CE_CMP_EXACT;r.value=L"1";r.rounding=CE_ROUND_ROUNDED;r.byte_length=0;r.alignment=0;
        CHECK(c.next_scan_v2(r)==CE_OK&&info(c).count==1);}
    {ce::Core c;Memory m(64);
        auto r=request(m,CE_TYPE_U32,L"1");r.rounding=CE_ROUND_ROUNDED;CHECK(c.first_scan_v2(r)==CE_INVALID_ARGUMENT);
        r=request(m,CE_TYPE_DOUBLE,L"1");r.rounding=CE_ROUND_ROUNDED;r.comparison=CE_CMP_GREATER;CHECK(c.first_scan_v2(r)==CE_INVALID_ARGUMENT);
        r=request(m,CE_TYPE_DOUBLE,L"1");r.rounding=4u;CHECK(c.first_scan_v2(r)==CE_INVALID_ARGUMENT);
        r=request(m,CE_TYPE_DOUBLE,L"1.00000000000000000000");CHECK(c.first_scan_v2(r)==CE_INVALID_ARGUMENT);}
}
void test_text_aob_and_large_records(){ce::Core c;Memory m(65536+8192);CHECK(c.write_value_v2(m.address(3),CE_TYPE_UTF8,0,0,L" aé \U0001F642 ")==CE_OK);auto r=request(m,CE_TYPE_UTF8,L" aé \U0001F642 ");CHECK(c.first_scan_v2(r)==CE_OK&&info(c).count==1&&result_address(c)==m.address(3));auto raw=result(c);CHECK(formatted(c,CE_TYPE_UTF8,0,raw.data(),uint32_t(raw.size()))==L" aé \U0001F642 ");r.comparison=CE_CMP_UNCHANGED;r.value=nullptr;CHECK(c.next_scan_v2(r)==CE_OK&&info(c).count==1);CHECK(c.write_value_v2(m.address(101),CE_TYPE_UTF16,0,0,L" x\U0001F642 ")==CE_OK);r=request(m,CE_TYPE_UTF16,L" x\U0001F642 ");CHECK(c.first_scan_v2(r)==CE_OK&&info(c).count==1&&result_address(c)==m.address(101));std::memset(m.p,0,m.n);m.p[10]=0xab;m.p[11]=0xab;m.p[12]=0xab;r=request(m,CE_TYPE_AOB,L"A? ?B");CHECK(c.first_scan_v2(r)==CE_OK&&info(c).count==2&&result_address(c)==m.address(10)&&result_address(c,1)==m.address(11));m.p[10]=0x77;r.comparison=CE_CMP_CHANGED;r.value=nullptr;CHECK(c.next_scan_v2(r)==CE_OK&&info(c).count==1);CHECK(c.write_value_v2(m.address(),CE_TYPE_AOB,0,0,L"A?")==CE_INVALID_ARGUMENT);
    std::memset(m.p,0,m.n);std::memset(m.p+100,0x5a,65536);std::wstring pattern;pattern.reserve(65536*3-1);for(size_t i=0;i<65536;++i){if(i)pattern+=L' ';pattern+=L"5A";}r=request(m,CE_TYPE_AOB,pattern.c_str());CHECK(c.first_scan_v2(r)==CE_OK&&info(c).count==1&&info(c).byte_length==65536&&result_address(c)==m.address(100));CHECK(result(c).size()==65536);r.comparison=CE_CMP_UNCHANGED;r.value=nullptr;CHECK(c.next_scan_v2(r)==CE_OK&&info(c).count==1&&result(c).size()==65536);CHECK(c.undo_scan_v2()==CE_OK&&result(c).size()==65536);DWORD old=0;CHECK(VirtualProtect(m.p+4096,4096,PAGE_NOACCESS,&old));r.comparison=CE_CMP_EXACT;r.value=pattern.c_str();CHECK(c.first_scan_v2(r)==CE_OK&&info(c).count==0);CHECK(VirtualProtect(m.p+4096,4096,PAGE_READWRITE,&old));}
uint64_t add(ce::Core& c,uint64_t address,uint32_t type,const wchar_t* value,const wchar_t* description=L"record",const CeAddressV2* target=nullptr){CeRecordRequestV2 r{};r.size=sizeof(r);r.version=2;r.type=type;r.address.base=address;if(target)r.address=*target;r.description=description;r.value=value;uint64_t id=0;CHECK(c.upsert_record_v2(r,id)==CE_OK&&id);return id;}
void test_records_pointers_ct(){ce::Core c;Memory m(4096);m.put<uint64_t>(0,8);auto id=add(c,m.address(),CE_TYPE_U64,L"99",L"wide & record");CHECK(m.get<uint64_t>()==8);CHECK(c.write_record_v2(id,L"42")==CE_OK&&m.get<uint64_t>()==42);CHECK(c.freeze_record_v2(id,true)==CE_OK);CHECK(c.freeze(m.address(4),7,true)==CE_INVALID_ARGUMENT);m.put<uint64_t>(0,1);c.tick_freezes();CHECK(m.get<uint64_t>()==42);CHECK(c.freeze_record_v2(id,false)==CE_OK);CHECK(c.freeze(m.address(4),7,true)==CE_OK);CHECK(c.freeze_record_v2(id,true)==CE_INVALID_ARGUMENT);CHECK(c.freeze(m.address(4),7,false)==CE_OK);
    uintptr_t pointer=static_cast<uintptr_t>(m.address(64));CeAddressV2 chain{};chain.base=reinterpret_cast<uintptr_t>(&pointer);chain.offset_count=1;chain.offsets[0]=4;uint64_t resolved=0;CHECK(c.resolve_pointer_v2(chain,resolved)==CE_OK&&resolved==m.address(68));auto pid=add(c,0,CE_TYPE_U16,L"123",L"pointer",&chain);CHECK(c.freeze_record_v2(pid,true)==CE_OK);pointer=static_cast<uintptr_t>(m.address(128));c.tick_freezes();CHECK(m.get<uint16_t>(132)==123);CHECK(c.new_scan_v2()==CE_OK);m.put<uint16_t>(132,2);c.tick_freezes();CHECK(m.get<uint16_t>(132)==123);File file;CHECK(c.save_table_v2(file.path.c_str())==CE_OK);CHECK(c.load_table_v2(file.path.c_str())==CE_OK);m.put<uint16_t>(132,4);c.tick_freezes();CHECK(m.get<uint16_t>(132)==4);uint64_t count=0;CHECK(c.record_count_v2(count)==CE_OK&&count==3);bool saw=false;for(uint64_t i=0;i<count;++i){CeRecordInfoV2 r{};std::wstring description;CHECK(c.record_v2(i,r,description)==CE_OK&&!r.frozen);if(r.id==pid){CHECK(r.address.offset_count==1&&r.address.offsets[0]==4&&description==L"pointer");saw=true;}}CHECK(saw);CHECK(c.save_table(file.path.c_str())==CE_UNSUPPORTED);// An unknown entry field no longer rejects the table: it is preserved and re-emitted.
    CHECK(c.remove_record_v2(id)==CE_OK);CHECK(c.remove_record_v2(id)==CE_INVALID_ARGUMENT);
    file.put("<CheatTable CheatEngineTableVersion=\"45\"><CheatEntries><CheatEntry><VariableType>4 Bytes</VariableType><Address>10000</Address><LastState Value=\"5\"/></CheatEntry></CheatEntries></CheatTable>");CHECK(c.load_table_v2(file.path.c_str())==CE_OK);CHECK(c.record_count_v2(count)==CE_OK&&count==1);CHECK(c.save_table_v2(file.path.c_str())==CE_OK);{const auto saved=file.get();CHECK(saved.find("LastState")!=std::string::npos&&saved.find("Value=\"5\"")!=std::string::npos);}CHECK(c.load_table_v2(file.path.c_str())==CE_OK&&c.record_count_v2(count)==CE_OK&&count==1);
    ce::Core strings;add(strings,m.address(300),CE_TYPE_UTF8,L" é ",L"utf8");add(strings,m.address(400),CE_TYPE_UTF16,L"\U0001F642",L"utf16");add(strings,m.address(500),CE_TYPE_AOB,L"00 FF AB",L"bytes");CHECK(strings.save_table_v2(file.path.c_str())==CE_OK);CHECK(strings.load_table_v2(file.path.c_str())==CE_OK);CHECK(strings.record_count_v2(count)==CE_OK&&count==3);}
std::string ct_entry(uint64_t address,const std::string& id){std::ostringstream out;out<<"<CheatEntry>"<<id<<"<VariableType>4 Bytes</VariableType><Address>"<<std::hex<<address<<"</Address></CheatEntry>";return out.str();}
std::string ct_document(const std::string& entries){return "<CheatTable CheatEngineTableVersion=\"45\"><CheatEntries>"+entries+"</CheatEntries></CheatTable>";}
std::string hex_address(uint64_t address){std::ostringstream out;out<<std::hex<<address;return out.str();}
// A foreign table must survive load/save intact: unknown entry fields, top-level
// siblings, and record types this build cannot represent are all kept verbatim.
void test_tolerant_ct(){
    ce::Core c;Memory m(4096);File file;uint64_t count=0;
    const auto module_base=reinterpret_cast<uint64_t>(GetModuleHandleW(L"kernel32.dll"));CHECK(module_base!=0);
    const std::string script="<CheatEntry><ID>7</ID><Description>infinite ammo</Description><VariableType>Auto Assembler Script</VariableType>"
        "<Address>\"kernel32.dll\"+1234</Address><AssemblerScript>[ENABLE]\nnop\n[DISABLE]\nnop\n// a &amp;&amp; b &lt; c > d</AssemblerScript></CheatEntry>";
    file.put(ct_document(ct_entry(m.address(),"<ID>1</ID>")+script));
    CHECK(c.load_table_v2(file.path.c_str())==CE_OK);CHECK(c.record_count_v2(count)==CE_OK&&count==2);
    CeRecordInfoV2 opaque{};std::wstring description;CHECK(c.record_v2(1,opaque,description)==CE_OK);
    // byte_length 0 plus CE_UNSUPPORTED is the unambiguous read-only marker.
    CHECK(opaque.id==7&&opaque.last_status==CE_UNSUPPORTED&&opaque.byte_length==0&&!opaque.frozen&&description==L"infinite ammo");
    CHECK(opaque.address.base==module_base+0x1234);
    CeRecordRequestV2 edit{};edit.size=sizeof(edit);edit.version=2;edit.type=CE_TYPE_U32;edit.flags=CE_VALUE_SIGNED;edit.id=7;edit.address.base=m.address();edit.value=L"9";uint64_t edited=0;
    CHECK(c.upsert_record_v2(edit,edited)==CE_UNSUPPORTED);
    CHECK(c.write_record_v2(7,L"9")==CE_UNSUPPORTED);
    CHECK(c.freeze_record_v2(7,true)==CE_UNSUPPORTED&&c.freeze_record_v2(7,false)==CE_UNSUPPORTED);
    CHECK(c.record_v2(1,opaque,description)==CE_OK&&opaque.id==7&&opaque.last_status==CE_UNSUPPORTED&&description==L"infinite ammo");
    CHECK(c.save_table_v2(file.path.c_str())==CE_OK);{const auto saved=file.get();
        CHECK(saved.find("<VariableType>Auto Assembler Script</VariableType>")!=std::string::npos);
        CHECK(saved.find("infinite ammo")!=std::string::npos);
        CHECK(saved.find("\"kernel32.dll\"+1234")!=std::string::npos);
        CHECK(saved.find("<AssemblerScript>")!=std::string::npos&&saved.find("[ENABLE]")!=std::string::npos);
        // Script bodies must come back byte-identical: indentation injected into a text
        // node would silently change the script.
        CHECK(saved.find("[ENABLE]\nnop\n[DISABLE]\nnop")!=std::string::npos);
        // Escaping must be applied by hand: the preserved text bypasses WriteString.
        CHECK(saved.find("// a &amp;&amp; b &lt; c &gt; d")!=std::string::npos);}
    CHECK(c.load_table_v2(file.path.c_str())==CE_OK&&c.record_count_v2(count)==CE_OK&&count==2);
    CHECK(c.record_v2(1,opaque,description)==CE_OK&&opaque.last_status==CE_UNSUPPORTED&&description==L"infinite ammo");
    // The V1 signed32 subset still refuses anything it cannot express.
    CHECK(c.save_table(file.path.c_str())==CE_UNSUPPORTED);
    CHECK(c.remove_record_v2(7)==CE_OK);
}
void test_ct_preserved_siblings(){
    ce::Core c;Memory m(4096);File file;uint64_t count=0;
    file.put("<CheatTable CheatEngineTableVersion=\"45\"><CheatEntries>"+ct_entry(m.address(),"<ID>3</ID>")+"</CheatEntries>"
        "<UserdefinedSymbols><Symbol>mySymbol</Symbol></UserdefinedSymbols><LuaScript>print(1)</LuaScript><Structures/></CheatTable>");
    CHECK(c.load_table_v2(file.path.c_str())==CE_OK&&c.record_count_v2(count)==CE_OK&&count==1);
    CHECK(c.save_table_v2(file.path.c_str())==CE_OK);{const auto saved=file.get();
        CHECK(saved.find("<UserdefinedSymbols>")!=std::string::npos&&saved.find("mySymbol")!=std::string::npos);
        CHECK(saved.find("<LuaScript>")!=std::string::npos&&saved.find("print(1)")!=std::string::npos);
        CHECK(saved.find("<Structures")!=std::string::npos);}
    CHECK(c.load_table_v2(file.path.c_str())==CE_OK&&c.record_count_v2(count)==CE_OK&&count==1);
}
// Editing an imported record keeps the foreign fields it came with.
void test_ct_unknown_field_survives_edit(){
    ce::Core c;Memory m(4096);File file;uint64_t count=0;
    file.put("<CheatTable CheatEngineTableVersion=\"45\"><CheatEntries><CheatEntry><ID>5</ID><VariableType>4 Bytes</VariableType><Address>"
        +hex_address(m.address())+"</Address><Options moHideChildren=\"1\"/><LastState Value=\"5\"/></CheatEntry></CheatEntries></CheatTable>");
    CHECK(c.load_table_v2(file.path.c_str())==CE_OK&&c.record_count_v2(count)==CE_OK&&count==1);
    CeRecordRequestV2 edit{};edit.size=sizeof(edit);edit.version=2;edit.type=CE_TYPE_U32;edit.flags=CE_VALUE_SIGNED;edit.id=5;edit.address.base=m.address();edit.value=L"12";uint64_t edited=0;
    CHECK(c.upsert_record_v2(edit,edited)==CE_OK&&edited==5);
    CHECK(c.save_table_v2(file.path.c_str())==CE_OK);{const auto saved=file.get();
        CHECK(saved.find("moHideChildren")!=std::string::npos);
        CHECK(saved.find("LastState")!=std::string::npos&&saved.find("Value=\"5\"")!=std::string::npos);}
}
// Tolerance must not turn into accepting malformed documents.
void test_ct_still_rejected(){
    const std::string open="<CheatTable CheatEngineTableVersion=\"45\"><CheatEntries>";
    const std::string close="</CheatEntries></CheatTable>";
    const std::string entry="<CheatEntry><VariableType>4 Bytes</VariableType><Address>10000</Address></CheatEntry>";
    const std::string cases[]={
        open+"<CheatEntry><VariableType>4 Bytes</VariableType><Address>10000</Address><Description><Nested/></Description></CheatEntry>"+close,
        open+"<CheatEntry><VariableType>4 Bytes</VariableType><VariableType>4 Bytes</VariableType><Address>10000</Address></CheatEntry>"+close,
        open+"<CheatEntry><VariableType>4 Bytes</VariableType></CheatEntry>"+close,
        open+"<CheatEntry><ID>4</ID><VariableType>4 Bytes</VariableType><Address>10000</Address></CheatEntry>"
             "<CheatEntry><ID>4</ID><VariableType>4 Bytes</VariableType><Address>10004</Address></CheatEntry>"+close,
        "<CheatTable CheatEngineTableVersion=\"44\"><CheatEntries>"+entry+close,
        open+"<!-- comment -->"+entry+close,
        open+"<![CDATA[x]]>"+entry+close,
        open+"<?pi x?>"+entry+close,
        open+entry+"</CheatEntries><CheatEntries>"+entry+"</CheatEntries></CheatTable>",
        "<CheatTable CheatEngineTableVersion=\"45\"><CheatEntries>"+entry+"</CheatEntries><LuaScript>"+std::string(4*1024*1024+1,'x')+"</LuaScript></CheatTable>"};
    for(const auto& document:cases){ce::Core c;Memory m(4096);File file;uint64_t count=0;add(c,m.address(),CE_TYPE_U32,L"1");
        file.put(document);const auto status=c.load_table_v2(file.path.c_str());
        CHECK(status==CE_INVALID_ARGUMENT||status==CE_UNSUPPORTED);
        CHECK(c.record_count_v2(count)==CE_OK&&count==1);}
}
// CE-style address expressions: + - * over hexadecimal numbers and loaded module
// names, with * binding first and + - evaluated strictly left to right.
void test_expressions(){
    ce::Core c;Memory m(4096);uint64_t value=0;
    const auto base=reinterpret_cast<uint64_t>(GetModuleHandleW(L"kernel32.dll"));CHECK(base!=0);
    auto resolve=[&](const wchar_t* text){return c.resolve_address_v2(text,value);};
    CHECK(resolve(L"10000")==CE_OK&&value==0x10000);
    CHECK(resolve(L"0x10000")==CE_OK&&value==0x10000);
    CHECK(resolve(L"$10000")==CE_OK&&value==0x10000);
    CHECK(resolve(L"  10000  ")==CE_OK&&value==0x10000);
    CHECK(resolve(L"10000+20")==CE_OK&&value==0x10020);
    CHECK(resolve(L"20000-10")==CE_OK&&value==0x1FFF0);
    CHECK(resolve(L"10000--10")==CE_OK&&value==0x10010);
    CHECK(resolve(L"1000*10")==CE_OK&&value==0x10000);
    CHECK(resolve(L"1000*10+4")==CE_OK&&value==0x10004);
    CHECK(resolve(L"4+1000*10")==CE_OK&&value==0x10004);
    // Every literal is hexadecimal, so 10*10 is 0x100, not one hundred.
    CHECK(resolve(L"10000+10*10")==CE_OK&&value==0x10100);
    CHECK(resolve(L"10000+2*3")==CE_OK&&value==0x10006);
    CHECK(resolve(L"10000+1000*10-8")==CE_OK&&value==0x1FFF8);
    CHECK(resolve(L"\"kernel32.dll\"")==CE_OK&&value==base);
    CHECK(resolve(L"kernel32.dll")==CE_OK&&value==base);
    CHECK(resolve(L"\"kernel32.dll\"+10")==CE_OK&&value==base+0x10);
    CHECK(resolve(L"\"kernel32.dll\"+10-8")==CE_OK&&value==base+8);
    CHECK(resolve(L"\"kernel32.dll\"-10")==CE_OK&&value==base-0x10);
    CHECK(resolve(L"\"kernel32.dll\"--10")==CE_OK&&value==base+0x10);
    const wchar_t* invalid[]{L"",L"   ",L"10000+",L"+",L"-",L"*",L"\"kernel32.dll",L"10000/2",L"(10000)",L"[10000]",
        L"not-a-valid-address-expression",L"0-1",L"FFFFFFFFFFFFFFFF+1",L"1000*10000000000000000",L"10000 20",L"no-such-module.exe"};
    for(const auto text:invalid)CHECK(resolve(text)==CE_INVALID_ARGUMENT);
    const std::wstring oversized(32701,L'1');CHECK(resolve(oversized.c_str())==CE_INVALID_ARGUMENT);
    // The CT loader shares the same evaluator, so a module expression survives import.
    ce::Core table;Memory table_memory(4096);File file;
    file.put("<CheatTable CheatEngineTableVersion=\"45\"><CheatEntries><CheatEntry><ID>9</ID><VariableType>4 Bytes</VariableType>"
        "<Address>\"kernel32.dll\"+10</Address></CheatEntry></CheatEntries></CheatTable>");
    CHECK(table.load_table_v2(file.path.c_str())==CE_OK);
    CeRecordInfoV2 record{};std::wstring description;CHECK(table.record_v2(0,record,description)==CE_OK);
    CHECK(record.resolved_address==base+0x10&&record.byte_length==4);
}
// Raw byte writes exist so callers can store bit patterns a parsed value cannot
// express, which is exactly what undoing a float edit needs.
void test_raw_bytes(){
    ce::Core c;Memory m(8);
    const uint8_t nan_payload[8]={0x01,0x00,0x00,0x00,0x00,0x00,0xF8,0x7F};
    CHECK(c.write_bytes_v2(m.address(),nan_payload,8)==CE_OK&&std::memcmp(m.p,nan_payload,8)==0);
    CHECK(std::isnan(m.get<double>()));
    CHECK(c.write_bytes_v2(m.address(),nullptr,8)==CE_INVALID_ARGUMENT);
    CHECK(c.write_bytes_v2(m.address(),nan_payload,0)==CE_INVALID_ARGUMENT);
    CHECK(c.write_bytes_v2(m.address(),nan_payload,CE_V2_MAX_VALUE_BYTES+1)==CE_INVALID_ARGUMENT);
    DWORD old=0;CHECK(VirtualProtect(m.p,m.n,PAGE_READONLY,&old));
    CHECK(c.write_bytes_v2(m.address(),nan_payload,8)==CE_ACCESS_ERROR);
    CHECK(VirtualProtect(m.p,m.n,PAGE_READWRITE,&old));
    c.shutdown();CHECK(c.write_bytes_v2(m.address(),nan_payload,8)==CE_NOT_RUNNING);
}
// The process views read host state and nothing else: no scan, no records, no lock.
void test_process_views(){
    ce::Core c;uint64_t sentinel=0;
    std::vector<CeRegionInfoV2> regions;CHECK(c.regions_v2(regions)==CE_OK&&!regions.empty());
    SYSTEM_INFO system{};GetSystemInfo(&system);
    bool tiled=true,covered=false;
    for(size_t i=0;i<regions.size();++i){CHECK(regions[i].size==sizeof(CeRegionInfoV2)&&regions[i].version==CE_V2_VERSION);
        const uint64_t base=regions[i].base,size=regions[i].region_size;CHECK(size>0);
        tiled&=i?base==regions[i-1].base+regions[i-1].region_size:base==reinterpret_cast<uintptr_t>(system.lpMinimumApplicationAddress);
        const auto here=reinterpret_cast<uint64_t>(&sentinel);
        if(here>=base&&here<base+size){covered=true;CHECK(size>=sizeof(sentinel));}}
    CHECK(tiled&&covered);
    std::vector<CeModuleInfoV2> modules;CHECK(c.modules_v2(modules)==CE_OK&&!modules.empty());
    const auto main_module=reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));bool found=false;
    for(const auto& m:modules){CHECK(m.size==sizeof(CeModuleInfoV2)&&m.version==CE_V2_VERSION);found|=m.base==main_module;}
    CHECK(found);
    std::vector<CeThreadInfoV2> threads;CHECK(c.threads_v2(threads)==CE_OK&&!threads.empty());
    unsigned current=0;
    for(const auto& t:threads){CHECK(t.size==sizeof(CeThreadInfoV2)&&t.version==CE_V2_VERSION);
        if(t.current){++current;CHECK(t.thread_id==GetCurrentThreadId());}}
    CHECK(current==1);
    // A stopped core refuses the views like every other operation.
    c.shutdown();CHECK(c.regions_v2(regions)==CE_NOT_RUNNING&&c.modules_v2(modules)==CE_NOT_RUNNING&&c.threads_v2(threads)==CE_NOT_RUNNING);
}
void test_ct_id_regression(){
    for(unsigned variant=0;variant<2;++variant){ce::Core c;Memory m(4096);File file;std::string entries=ct_entry(m.address(),"<ID>2</ID>");if(variant)entries+=ct_entry(m.address(16),"<ID>0</ID>")+ct_entry(m.address(32),"");file.put(ct_document(entries));CHECK(c.load_table(file.path.c_str())==CE_OK);auto added=add(c,m.address(64),CE_TYPE_U64,L"7");CHECK(added!=2);CHECK(c.save_table_v2(file.path.c_str())==CE_OK);CHECK(c.load_table_v2(file.path.c_str())==CE_OK);uint64_t count=0;CHECK(c.record_count_v2(count)==CE_OK&&count==(variant?4u:2u));std::set<uint64_t> ids;bool original=false,created=false;for(uint64_t i=0;i<count;++i){CeRecordInfoV2 r{};std::wstring description;CHECK(c.record_v2(i,r,description)==CE_OK&&r.id&&ids.insert(r.id).second);original|=r.id==2;created|=r.id==added;}CHECK(original&&created);CHECK(c.save_table_v2(file.path.c_str())==CE_OK);CHECK(c.load_table_v2(file.path.c_str())==CE_OK);}
}
void test_invalid_ct_snapshot_freeze(){
    for(bool legacy:{false,true}){ce::Core c;Memory m(4096);File file;file.put(ct_document(ct_entry(m.address(),"<ID>2</ID>")));DWORD old=0;CHECK(VirtualProtect(m.p,m.n,PAGE_NOACCESS,&old));CHECK((legacy?c.load_table(file.path.c_str()):c.load_table_v2(file.path.c_str()))==CE_OK);CeRecordInfoV2 r{};std::wstring description;CHECK(c.record_v2(0,r,description)==CE_OK&&!r.frozen&&r.last_status==CE_ACCESS_ERROR);const auto id=r.id;CHECK(c.freeze_record_v2(id,true)==CE_ACCESS_ERROR);CHECK(c.freeze_record_v2(id,false)==CE_OK);CHECK(c.record_v2(0,r,description)==CE_OK&&!r.frozen&&r.last_status==CE_ACCESS_ERROR);CHECK(VirtualProtect(m.p,m.n,PAGE_READWRITE,&old));m.put<int32_t>(0,987654);CHECK(c.freeze_record_v2(id,true)==CE_OK);c.tick_freezes();CHECK(m.get<int32_t>()==987654);m.put<int32_t>(0,42);c.tick_freezes();CHECK(m.get<int32_t>()==987654);CHECK(c.freeze_record_v2(id,false)==CE_OK);
        CHECK(VirtualProtect(m.p,m.n,PAGE_NOACCESS,&old));CHECK((legacy?c.load_table(file.path.c_str()):c.load_table_v2(file.path.c_str()))==CE_OK);CHECK(c.record_v2(0,r,description)==CE_OK&&r.last_status==CE_ACCESS_ERROR);CHECK(VirtualProtect(m.p,m.n,PAGE_READWRITE,&old));CHECK(c.write_record_v2(r.id,L"12345")==CE_OK);CHECK(c.freeze_record_v2(r.id,true)==CE_OK);m.put<int32_t>(0,8);c.tick_freezes();CHECK(m.get<int32_t>()==12345);
        CHECK(VirtualProtect(m.p,m.n,PAGE_NOACCESS,&old));CHECK((legacy?c.load_table(file.path.c_str()):c.load_table_v2(file.path.c_str()))==CE_OK);CHECK(c.record_v2(0,r,description)==CE_OK&&r.last_status==CE_ACCESS_ERROR);CHECK(VirtualProtect(m.p,m.n,PAGE_READWRITE,&old));CeRecordRequestV2 edit{};edit.size=sizeof(edit);edit.version=2;edit.type=CE_TYPE_U32;edit.flags=CE_VALUE_SIGNED;edit.id=r.id;edit.address.base=m.address();edit.value=L"6789";uint64_t edited=0;CHECK(c.upsert_record_v2(edit,edited)==CE_OK&&edited==r.id);CHECK(c.freeze_record_v2(edited,true)==CE_OK);m.put<int32_t>(0,8);c.tick_freezes();CHECK(m.get<int32_t>()==6789);
        for(unsigned correction=0;correction<3;++correction){CHECK(VirtualProtect(m.p,m.n,PAGE_NOACCESS,&old));CHECK((legacy?c.load_table(file.path.c_str()):c.load_table_v2(file.path.c_str()))==CE_OK);CHECK(c.record_v2(0,r,description)==CE_OK&&r.last_status==CE_ACCESS_ERROR);CHECK(VirtualProtect(m.p,m.n,PAGE_READWRITE,&old));m.put<int32_t>(0,2468);if(correction==0){edit.id=r.id;edit.value=nullptr;CHECK(c.upsert_record_v2(edit,edited)==CE_OK);}else if(correction==1){CHECK(c.write(m.address(),2468)==CE_OK);}else{CHECK(c.freeze(m.address(),2468,true)==CE_OK);}CHECK(c.freeze_record_v2(r.id,true)==CE_OK);m.put<int32_t>(0,1);c.tick_freezes();CHECK(m.get<int32_t>()==2468);}
    }
}
void test_numeric_type_matrix(){
    const uint32_t widths[]={1,2,4,8,4,8};for(uint32_t type=CE_TYPE_U8;type<=CE_TYPE_DOUBLE;++type){ce::Core c;Memory m(widths[type]*3);for(uint32_t i=0;i<3;++i){auto text=std::to_wstring(i+1);CHECK(c.write_value_v2(m.address(i*widths[type]),type,0,0,text.c_str())==CE_OK);}std::vector<uint8_t> bytes(widths[type]);CHECK(c.read_bytes_v2(m.address(),bytes.data(),widths[type])==CE_OK);CHECK(formatted(c,type,0,bytes.data(),widths[type])==L"1");auto r=request(m,type,nullptr,CE_CMP_UNKNOWN,0,widths[type]);CHECK(c.first_scan_v2(r)==CE_OK&&info(c).count==3);r.comparison=CE_CMP_LESS;r.value=L"3";CHECK(c.next_scan_v2(r)==CE_OK&&info(c).count==2);r.comparison=CE_CMP_BETWEEN;r.value=L"1";r.value2=L"2";CHECK(c.next_scan_v2(r)==CE_OK&&info(c).count==2);r.comparison=CE_CMP_EXACT;r.value=L"2";r.value2=nullptr;CHECK(c.next_scan_v2(r)==CE_OK&&info(c).count==1&&result_address(c)==m.address(widths[type]));bytes=result(c);CHECK(formatted(c,type,0,bytes.data(),widths[type])==L"2");}
}
void test_cancel_metadata(){
    ce::Core c; Memory small(16),large(64*1024*1024); small.put<uint64_t>(0,99);
    auto r=request(small,CE_TYPE_U64,L"99",CE_CMP_EXACT,0,8);
    bool cancelled=false;
    for(unsigned attempt=0;attempt<4&&!cancelled;++attempt){
        CHECK(c.first_scan_v2(r)==CE_OK); const auto before=info(c);
        std::atomic<bool> done{false}; std::atomic<int> result_code{CE_INTERNAL_ERROR};
        std::thread worker([&]{auto q=request(large,CE_TYPE_U8,nullptr,CE_CMP_UNKNOWN);result_code=c.first_scan_v2(q);done=true;});
        // Observe instead of trying to start another invalid scan: the old busy
        // probe could acquire admission first and cause the actual worker BUSY.
        while(!done.load()){
            CeScanStatusV2 status{};status.size=sizeof(status);status.version=CE_V2_VERSION;
            if(c.scan_status_v2(status)==CE_OK&&status.active)c.cancel_scan();
            Sleep(1);
        }
        worker.join();
        if(result_code==CE_OK){CHECK(info(c).type==CE_TYPE_U8);continue;}
        CHECK(result_code==CE_CANCELLED); const auto after=info(c);
        CHECK(before.generation==after.generation&&before.type==after.type&&before.byte_length==after.byte_length&&before.count==after.count);
        cancelled=true;
    }
    CHECK(cancelled); c.request_stop(); CHECK(c.first_scan_v2(r)==CE_NOT_RUNNING);
    CHECK(c.new_scan_v2()==CE_NOT_RUNNING); CHECK(c.undo_scan_v2()==CE_NOT_RUNNING);
}
}
// Round-trip a multi-level pointer chain through CT save/load. A single offset
// cannot catch ordering mistakes because reversal is the identity there.
void test_ct_multi_offset_roundtrip(){
    ce::Core c;Memory m(4096);File file;
    // A resolvable 3-hop chain: read the pointer at each hop, apply the offset.
    m.put<uint64_t>(0,m.address(64));      // hop 0: pointer to hop 1 storage
    m.put<uint64_t>(64,m.address(128));    // hop 1: pointer to hop 2 storage
    m.put<uint64_t>(128,m.address(192));   // hop 2: pointer to final location
    CeAddressV2 chain{};chain.base=m.address();
    chain.offset_count=3;chain.offsets[0]=0;chain.offsets[1]=0;chain.offsets[2]=4;
    uint64_t resolved=0;CHECK(c.resolve_pointer_v2(chain,resolved)==CE_OK&&resolved==m.address(196));
    add(c,0,CE_TYPE_U32,L"7",L"chain",&chain);
    CHECK(c.save_table_v2(file.path.c_str())==CE_OK);
    CHECK(c.load_table_v2(file.path.c_str())==CE_OK);
    uint64_t count=0;CHECK(c.record_count_v2(count)==CE_OK&&count==1);
    CeRecordInfoV2 r{};std::wstring description;CHECK(c.record_v2(0,r,description)==CE_OK);
    CHECK(r.address.offset_count==3&&r.address.offsets[0]==0&&r.address.offsets[1]==0&&r.address.offsets[2]==4);
    CHECK(r.resolved_address==m.address(196));
}
int main(){try{test_integer_limits();test_modes_and_history();test_integer_delta_overflow();test_float();test_float_rounding();test_text_aob_and_large_records();test_records_pointers_ct();test_ct_multi_offset_roundtrip();test_ct_id_regression();test_tolerant_ct();test_ct_preserved_siblings();test_ct_unknown_field_survives_edit();test_ct_still_rejected();test_expressions();test_process_views();test_raw_bytes();test_invalid_ct_snapshot_freeze();test_numeric_type_matrix();test_cancel_metadata();std::cout<<"All typed core tests passed.\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::wcerr<<ce::last_error()<<'\n';return 1;}}
