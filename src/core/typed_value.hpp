#pragma once
#include "ce/api_v2.h"
#include <cstdint>
#include <string>
#include <vector>
namespace ce::typed {
// precision is CE's floataccuracy: how many digits the caller typed after the decimal
// separator. It selects the rounding bucket, so it travels with the parsed value.
struct Value { uint32_t type=0, flags=0, width=0, precision=0; std::vector<uint8_t> bytes, mask; };
uint32_t native_width(uint32_t type);
bool numeric(uint32_t type);
int layout(uint32_t type,uint32_t flags,uint32_t width);
int parse(uint32_t type,uint32_t flags,uint32_t width,const wchar_t* text,Value& out,bool wildcards=false);
int format(uint32_t type,uint32_t flags,const void* bytes,uint32_t width,std::wstring& out);
int prepare(const CeScanRequestV2& request,bool next,uint32_t inherited,Value& a,Value& b,uint32_t& width);
bool matches(uint32_t comparison,uint32_t type,uint32_t flags,uint32_t width,const uint8_t* current,const uint8_t* previous,const Value& a,const Value& b,uint32_t rounding);
bool valid_range(uint64_t address,uint32_t width);
bool accessible(uint64_t address,uint32_t width,bool writing);
bool read(uint64_t address,void* bytes,uint32_t width);
bool write(uint64_t address,const void* bytes,uint32_t width);
int resolve(const CeAddressV2& address,uint64_t& result);
int expression(const wchar_t* expression,uint64_t& address);
}
