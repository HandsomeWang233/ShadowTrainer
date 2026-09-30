#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "typed_value.hpp"
#include "core.hpp"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <iomanip>
#include <limits>
#include <locale>
#include <locale.h>
#include <sstream>
namespace ce::typed {
namespace {
int error(int code,const wchar_t* text){set_error(text);return code;}
std::wstring trim(std::wstring s){auto p=s.find_first_not_of(L" \t\r\n");return p==s.npos?L"":s.substr(p,s.find_last_not_of(L" \t\r\n")-p+1);}
int digit(wchar_t c){if(c>=L'0'&&c<=L'9')return c-L'0';if(c>=L'a'&&c<=L'f')return c-L'a'+10;if(c>=L'A'&&c<=L'F')return c-L'A'+10;return -1;}
bool uint_text(std::wstring s,unsigned base,uint64_t& n){if(base==16&&s.size()>2&&s[0]==L'0'&&(s[1]==L'x'||s[1]==L'X'))s.erase(0,2);if(s.empty())return false;n=0;for(auto c:s){int d=digit(c);if(d<0||unsigned(d)>=base||n>(UINT64_MAX-unsigned(d))/base)return false;n=n*base+unsigned(d);}return true;}
uint64_t bits(const uint8_t* p,uint32_t width){uint64_t n=0;std::memcpy(&n,p,width);return n;}
int64_t signed_bits(const uint8_t* p,uint32_t width){uint64_t n=bits(p,width);if(width<8&&(n&(uint64_t(1)<<(width*8-1))))n|=(~uint64_t(0))<<(width*8);int64_t r;std::memcpy(&r,&n,8);return r;}
long double real(const uint8_t* p,uint32_t type){if(type==CE_TYPE_FLOAT){float v;std::memcpy(&v,p,4);return v;}double v;std::memcpy(&v,p,8);return v;}
bool unicode(const std::wstring& s){for(size_t i=0;i<s.size();++i){unsigned c=s[i];if(c>=0xd800&&c<=0xdbff){if(++i==s.size()||s[i]<0xdc00||s[i]>0xdfff)return false;}else if(c>=0xdc00&&c<=0xdfff)return false;}return true;}
long double power_of_ten(int n){long double scale=1;for(int i=0;i<n;++i)scale*=10;return scale;}
// Round half to even at precision n, the way Delphi's RoundTo does. The ambient FPU
// rounding mode is deliberately never consulted: the host process can change it.
bool round_half_even(long double x,int n,long double& out){const long double scale=power_of_ten(n);x*=scale;if(!std::isfinite(x)||std::fabs(x)>=9223372036854775808.0L)return false;long double whole=std::floor(x),fraction=x-whole;if(fraction>0.5L||(fraction==0.5L&&std::fmod(whole,2.0L)!=0.0L))whole+=1;out=whole/scale;return true;}
// CE's three exact-float comparisons. Extreme rounding really is asymmetric there:
// SingleExact uses a closed interval while DoubleExact uses an open one.
bool rounded(long double current,long double entered,uint32_t precision,uint32_t type,uint32_t rounding){
    if(rounding==CE_ROUND_ROUNDED){long double bucket=0;return round_half_even(current,int(precision),bucket)&&bucket==entered;}
    const long double unit=1/power_of_ten(int(precision));
    if(rounding==CE_ROUND_TRUNCATED)return current>=entered&&current<entered+unit;
    return type==CE_TYPE_FLOAT?(current>=entered-unit&&current<=entered+unit):(current>entered-unit&&current<entered+unit);
}
template<class T> bool compare(uint32_t mode,T c,T p,T a,T b){switch(mode){case CE_CMP_EXACT:return c==a;case CE_CMP_GREATER:return c>a;case CE_CMP_LESS:return c<a;case CE_CMP_BETWEEN:return c>=a&&c<=b;case CE_CMP_INCREASED:return c>p;case CE_CMP_DECREASED:return c<p;default:return false;}}
bool readable(DWORD p,bool w){if(p&(PAGE_GUARD|PAGE_NOACCESS))return false;p&=255;return p==PAGE_READWRITE||p==PAGE_WRITECOPY||p==PAGE_EXECUTE_READWRITE||p==PAGE_EXECUTE_WRITECOPY||(!w&&(p==PAGE_READONLY||p==PAGE_EXECUTE_READ));}
}
uint32_t native_width(uint32_t t){static const uint32_t widths[]={1,2,4,8,4,8,0,0,0};return t<=CE_TYPE_AOB?widths[t]:0;}
bool numeric(uint32_t t){return t<=CE_TYPE_DOUBLE;}
int layout(uint32_t t,uint32_t f,uint32_t w){if(t>CE_TYPE_AOB||(f&~uint32_t(CE_VALUE_SIGNED|CE_VALUE_HEX))||(t>CE_TYPE_U64&&f)||w>CE_V2_MAX_VALUE_BYTES||(native_width(t)&&w&&w!=native_width(t))||(t==CE_TYPE_UTF16&&(w&1)))return error(CE_INVALID_ARGUMENT,L"Invalid value type, flags or byte width.");return CE_OK;}
int parse(uint32_t t,uint32_t f,uint32_t w,const wchar_t* text,Value& out,bool wildcards){
    int status=layout(t,f,w);if(status)return status;if(!text)return error(CE_INVALID_ARGUMENT,L"This comparison requires a value.");
    size_t len=0;while(text[len]&&len<=CE_V2_MAX_VALUE_BYTES*3u)++len;if(len>CE_V2_MAX_VALUE_BYTES*3u)return error(CE_INVALID_ARGUMENT,L"Value text exceeds the 64 KiB byte limit.");
    std::wstring s(text,len);Value v;v.type=t;v.flags=f;
    if(t<=CE_TYPE_U64){s=trim(s);bool negative=false;if(!s.empty()&&(s[0]==L'-'||s[0]==L'+')){negative=s[0]==L'-';s.erase(0,1);}bool hex=(f&CE_VALUE_HEX)||(s.size()>2&&s[0]==L'0'&&(s[1]==L'x'||s[1]==L'X'));uint64_t n=0;uint32_t width=native_width(t);uint64_t max=width==8?UINT64_MAX:(uint64_t(1)<<(width*8))-1;
        if(!uint_text(s,hex?16:10,n)||n>max|| (negative&&(!(f&CE_VALUE_SIGNED)||hex)))return error(CE_INVALID_ARGUMENT,L"Integer is invalid or outside its exact width.");
        if(!hex&&(f&CE_VALUE_SIGNED)){uint64_t limit=uint64_t(1)<<(width*8-1);if(n>limit-(!negative))return error(CE_INVALID_ARGUMENT,L"Signed integer overflow.");}if(negative)n=uint64_t(0)-n;v.bytes.resize(width);std::memcpy(v.bytes.data(),&n,width);
    }else if(t<=CE_TYPE_DOUBLE){s=trim(s);if(s.empty())return error(CE_INVALID_ARGUMENT,L"Empty floating point value.");wchar_t* end=nullptr;errno=0;_locale_t locale=_create_locale(LC_NUMERIC,"C");if(!locale)return error(CE_INTERNAL_ERROR,L"Cannot create numeric parsing locale.");double n=_wcstod_l(s.c_str(),&end,locale);_free_locale(locale);if(end!=s.c_str()+s.size()||(errno==ERANGE&&!std::isfinite(n)))return error(CE_INVALID_ARGUMENT,L"Invalid or overflowing floating point value.");
        // CE derives floataccuracy from the typed text: digits after the separator, or 0
        // whenever the text carries an exponent. inf/nan have no fraction either.
        if(s.find_first_of(L"eE")==s.npos){const size_t dot=s.find(L'.');if(dot!=s.npos){const size_t digits=s.size()-dot-1;if(digits>19)return error(CE_INVALID_ARGUMENT,L"Floating point value has more than 19 fractional digits.");v.precision=uint32_t(digits);}}
        if(t==CE_TYPE_FLOAT){float q=static_cast<float>(n);if(std::isfinite(n)&&!std::isfinite(q))return error(CE_INVALID_ARGUMENT,L"Float overflow.");v.bytes.resize(4);std::memcpy(v.bytes.data(),&q,4);}else{v.bytes.resize(8);std::memcpy(v.bytes.data(),&n,8);}
    }else if(t==CE_TYPE_AOB){std::wistringstream in(s);std::wstring token;while(in>>token){if(token==L"?")token=L"??";if(token.size()!=2)return error(CE_INVALID_ARGUMENT,L"AOB needs whitespace-separated pairs of hexadecimal digits or ? nibbles.");uint8_t byte=0,mask=0;for(auto c:token){byte<<=4;mask<<=4;if(c==L'?'){if(!wildcards)return error(CE_INVALID_ARGUMENT,L"Wildcards are allowed only in scan patterns.");}else{int d=digit(c);if(d<0)return error(CE_INVALID_ARGUMENT,L"Invalid AOB nibble.");byte|=uint8_t(d);mask|=15;}}v.bytes.push_back(byte);v.mask.push_back(mask);if(v.bytes.size()>CE_V2_MAX_VALUE_BYTES)return error(CE_INVALID_ARGUMENT,L"Value exceeds 64 KiB.");}
    }else{if(!unicode(s))return error(CE_INVALID_ARGUMENT,L"Text is not valid UTF-16.");if(t==CE_TYPE_UTF16){v.bytes.resize(s.size()*2);if(!v.bytes.empty())std::memcpy(v.bytes.data(),s.data(),v.bytes.size());}else{int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),int(s.size()),nullptr,0,nullptr,nullptr);if(size<0||(!size&&!s.empty()))return error(CE_INVALID_ARGUMENT,L"Cannot encode UTF-8.");v.bytes.resize(size);if(size)WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),int(s.size()),reinterpret_cast<char*>(v.bytes.data()),size,nullptr,nullptr);}}
    if(v.bytes.empty()||v.bytes.size()>CE_V2_MAX_VALUE_BYTES||(w&&w!=v.bytes.size()))return error(CE_INVALID_ARGUMENT,L"Value byte length must be 1..65536 and exactly match the requested width.");v.width=uint32_t(v.bytes.size());out=std::move(v);return CE_OK;
}
int format(uint32_t t,uint32_t f,const void* data,uint32_t w,std::wstring& out){int s=layout(t,f,w);if(s)return s;if(!data||!w||(native_width(t)&&w!=native_width(t)))return error(CE_INVALID_ARGUMENT,L"Invalid format buffer.");auto p=static_cast<const uint8_t*>(data);std::wostringstream stream;stream.imbue(std::locale::classic());
    if(t<=CE_TYPE_U64){if(f&CE_VALUE_HEX)stream<<std::uppercase<<std::hex<<std::setfill(L'0')<<std::setw(w*2)<<bits(p,w);else if(f&CE_VALUE_SIGNED)stream<<signed_bits(p,w);else stream<<bits(p,w);}
    else if(t<=CE_TYPE_DOUBLE)stream<<std::setprecision(t==CE_TYPE_FLOAT?std::numeric_limits<float>::max_digits10:std::numeric_limits<double>::max_digits10)<<real(p,t);
    else if(t==CE_TYPE_AOB){for(uint32_t i=0;i<w;++i){if(i)stream<<L' ';stream<<std::uppercase<<std::hex<<std::setfill(L'0')<<std::setw(2)<<unsigned(p[i]);}}
    else if(t==CE_TYPE_UTF16){std::wstring text(w/2,L'\0');std::memcpy(text.data(),p,w);if(!unicode(text))return error(CE_INVALID_ARGUMENT,L"Memory is not valid UTF-16.");out=std::move(text);return CE_OK;}
    else{int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,reinterpret_cast<const char*>(p),w,nullptr,0);if(!n)return error(CE_INVALID_ARGUMENT,L"Memory is not valid UTF-8.");std::wstring text(n,L'\0');MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,reinterpret_cast<const char*>(p),w,text.data(),n);out=std::move(text);return CE_OK;}out=stream.str();return CE_OK;
}
int prepare(const CeScanRequestV2& r,bool next,uint32_t inherited,Value& a,Value& b,uint32_t& width){
    if(r.size!=sizeof(r)||r.version!=CE_V2_VERSION||r.rounding>CE_ROUND_TRUNCATED||r.comparison>CE_CMP_DECREASED_BY||!r.alignment||r.alignment>CE_V2_MAX_VALUE_BYTES||(r.alignment&(r.alignment-1)))return error(CE_INVALID_ARGUMENT,L"Invalid scan structure, comparison, rounding or power-of-two alignment.");
    if(r.rounding!=CE_ROUND_EXACT&&(r.comparison!=CE_CMP_EXACT||(r.type!=CE_TYPE_FLOAT&&r.type!=CE_TYPE_DOUBLE)))return error(CE_INVALID_ARGUMENT,L"Rounding applies only to exact Float or Double scans.");
    int s=layout(r.type,r.flags,r.byte_length);if(s)return s;if(!next&&r.comparison>CE_CMP_BETWEEN)return error(CE_INVALID_ARGUMENT,L"History comparisons require a next scan.");if(next&&r.comparison==CE_CMP_UNKNOWN)return error(CE_INVALID_ARGUMENT,L"Unknown is a first-scan comparison only.");
    if(!numeric(r.type)&&r.comparison!=CE_CMP_EXACT&&r.comparison!=CE_CMP_CHANGED&&r.comparison!=CE_CMP_UNCHANGED)return error(CE_UNSUPPORTED,L"Text/AOB support exact, changed and unchanged only.");
    width=r.byte_length?r.byte_length:(next?inherited:native_width(r.type));bool needs=r.comparison==CE_CMP_EXACT||r.comparison==CE_CMP_GREATER||r.comparison==CE_CMP_LESS||r.comparison==CE_CMP_BETWEEN||r.comparison>=CE_CMP_INCREASED_BY;
    if(needs){s=parse(r.type,r.flags,width,r.value,a,r.comparison==CE_CMP_EXACT);if(s)return s;width=a.width;}
    if(!width)return error(CE_INVALID_ARGUMENT,L"A fixed nonzero value width is required.");
    if(r.comparison==CE_CMP_BETWEEN){s=parse(r.type,r.flags,width,r.value2,b);if(s)return s;if(matches(CE_CMP_GREATER,r.type,r.flags,width,a.bytes.data(),nullptr,b,b,CE_ROUND_EXACT))return error(CE_INVALID_ARGUMENT,L"Between lower bound exceeds upper bound.");}
    if(r.comparison>=CE_CMP_INCREASED_BY&&r.type>=CE_TYPE_FLOAT&&!std::isfinite(real(a.bytes.data(),r.type)))return error(CE_INVALID_ARGUMENT,L"Delta must be finite.");return CE_OK;
}
bool matches(uint32_t mode,uint32_t t,uint32_t f,uint32_t w,const uint8_t* c,const uint8_t* p,const Value& a,const Value& b,uint32_t rounding){
    if(mode==CE_CMP_UNKNOWN)return true;if(mode==CE_CMP_CHANGED)return std::memcmp(c,p,w)!=0;if(mode==CE_CMP_UNCHANGED)return std::memcmp(c,p,w)==0;
    if(!numeric(t)){for(uint32_t i=0;i<w;++i){uint8_t mask=a.mask.empty()?255:a.mask[i];if((c[i]&mask)!=(a.bytes[i]&mask))return false;}return true;}
    if(t<=CE_TYPE_U64){if(f&CE_VALUE_SIGNED){int64_t cv=signed_bits(c,w),pv=p?signed_bits(p,w):0,av=a.bytes.empty()?0:signed_bits(a.bytes.data(),w),bv=b.bytes.empty()?0:signed_bits(b.bytes.data(),w);if(mode>=CE_CMP_INCREASED_BY){int64_t low=w==8?INT64_MIN:-(int64_t(1)<<(w*8-1)),high=w==8?INT64_MAX:(int64_t(1)<<(w*8-1))-1;if(mode==CE_CMP_INCREASED_BY){if((av>0&&pv>high-av)||(av<0&&pv<low-av))return false;return cv==pv+av;}if((av>0&&pv<low+av)||(av<0&&pv>high+av))return false;return cv==pv-av;}return compare(mode,cv,pv,av,bv);}
        uint64_t cv=bits(c,w),pv=p?bits(p,w):0,av=a.bytes.empty()?0:bits(a.bytes.data(),w),bv=b.bytes.empty()?0:bits(b.bytes.data(),w),high=w==8?UINT64_MAX:(uint64_t(1)<<(w*8))-1;if(mode==CE_CMP_INCREASED_BY)return av<=high-pv&&cv==pv+av;if(mode==CE_CMP_DECREASED_BY)return av<=pv&&cv==pv-av;return compare(mode,cv,pv,av,bv);}
    long double cv=real(c,t),pv=p?real(p,t):0,av=a.bytes.empty()?0:real(a.bytes.data(),t),bv=b.bytes.empty()?0:real(b.bytes.data(),t);if(mode==CE_CMP_EXACT&&rounding!=CE_ROUND_EXACT)return rounded(cv,av,a.precision,t,rounding);if(mode>=CE_CMP_INCREASED_BY){if(!std::isfinite(cv)||!std::isfinite(pv)||!std::isfinite(av))return false;long double target=mode==CE_CMP_INCREASED_BY?pv+av:pv-av;if(t==CE_TYPE_FLOAT){float v=static_cast<float>(target);return std::isfinite(v)&&cv==v;}double v=static_cast<double>(target);return std::isfinite(v)&&cv==v;}return compare(mode,cv,pv,av,bv);
}
bool valid_range(uint64_t a,uint32_t w){SYSTEM_INFO s{};GetSystemInfo(&s);uint64_t max=reinterpret_cast<uintptr_t>(s.lpMaximumApplicationAddress);return w&&w<=CE_V2_MAX_VALUE_BYTES&&a>=reinterpret_cast<uintptr_t>(s.lpMinimumApplicationAddress)&&a<=max&&w-1<=max-a;}
bool accessible(uint64_t a,uint32_t w,bool writing){if(!valid_range(a,w))return false;uint64_t end=a+w;while(a<end){MEMORY_BASIC_INFORMATION m{};if(!VirtualQuery(reinterpret_cast<void*>(uintptr_t(a)),&m,sizeof(m))||m.State!=MEM_COMMIT||!readable(m.Protect,writing))return false;uint64_t n=reinterpret_cast<uintptr_t>(m.BaseAddress)+m.RegionSize;if(n<=a)return false;a=(std::min)(n,end);}return true;}
bool read(uint64_t a,void* p,uint32_t w){if(!p||!accessible(a,w,false))return false;SIZE_T n=0;return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(uintptr_t(a)),p,w,&n)&&n==w;}
bool write(uint64_t a,const void* p,uint32_t w){if(!p||!accessible(a,w,true))return false;SIZE_T n=0;return WriteProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(uintptr_t(a)),p,w,&n)&&n==w;}
int resolve(const CeAddressV2& a,uint64_t& result){if(a.reserved||a.offset_count>CE_V2_MAX_OFFSETS)return error(CE_INVALID_ARGUMENT,L"Invalid pointer-chain structure.");uint64_t v=a.base;if(!valid_range(v,1))return error(CE_INVALID_ARGUMENT,L"Invalid base address.");for(uint32_t i=0;i<a.offset_count;++i){uintptr_t pointer=0;if(!read(v,&pointer,sizeof(pointer)))return error(CE_ACCESS_ERROR,L"Cannot read a pointer-chain link.");uint64_t n=pointer;int64_t off=a.offsets[i];uint64_t magnitude=off<0?uint64_t(0)-uint64_t(off):uint64_t(off);if((off<0&&n<magnitude)||(off>=0&&n>UINT64_MAX-magnitude))return error(CE_INVALID_ARGUMENT,L"Pointer offset overflow.");v=off<0?n-magnitude:n+magnitude;if(!valid_range(v,1))return error(CE_INVALID_ARGUMENT,L"Pointer result is outside the host address space.");}result=v;return CE_OK;}
// CE's symbolhandler.getAddressFromName numeric/module subset: hexadecimal literals and
// loaded module names joined by + - *, where * binds first and + - run left to right.
// Symbols, dereference, casts and parentheses are deliberately absent.
int expression(const wchar_t* text,uint64_t& address){
    if(!text||std::wcslen(text)>32700)return error(CE_INVALID_ARGUMENT,L"Invalid address expression.");std::wstring s=trim(text);
    if(s.empty())return error(CE_INVALID_ARGUMENT,L"Empty address expression.");size_t at=0;
    auto skip=[&]{while(at<s.size()&&(s[at]==L' '||s[at]==L'\t'||s[at]==L'\r'||s[at]==L'\n'))++at;};
    auto operand=[&](uint64_t& value)->int{
        skip();if(at>=s.size())return error(CE_INVALID_ARGUMENT,L"Address expression ends with an operator.");
        if(s[at]==L'\"'){const size_t close=s.find(L'\"',at+1);if(close==s.npos)return error(CE_INVALID_ARGUMENT,L"Unterminated module name.");const std::wstring module_name=s.substr(at+1,close-at-1);at=close+1;
            const HMODULE module=GetModuleHandleW(module_name.c_str());if(!module)return error(CE_INVALID_ARGUMENT,L"Address expression names a module that is not loaded.");value=reinterpret_cast<uintptr_t>(module);return CE_OK;}
        if(s[at]==L'+'||s[at]==L'-'||s[at]==L'*')return error(CE_INVALID_ARGUMENT,L"Address expression has a missing operand.");
        std::wstring token;
        if(s[at]==L'$'){++at;while(at<s.size()&&digit(s[at])>=0)token.push_back(s[at++]);}
        else{const size_t start=at;while(at<s.size()&&s[at]!=L'+'&&s[at]!=L'-'&&s[at]!=L'*'&&s[at]!=L'\"'&&s[at]!=L' '&&s[at]!=L'\t'&&s[at]!=L'\r'&&s[at]!=L'\n')++at;token=s.substr(start,at-start);}
        if(token.empty())return error(CE_INVALID_ARGUMENT,L"Address expression has a missing operand.");
        if(uint_text(token,16,value))return CE_OK;
        const HMODULE module=GetModuleHandleW(token.c_str());if(!module)return error(CE_INVALID_ARGUMENT,L"Address expression needs a hexadecimal number or a loaded module name.");
        value=reinterpret_cast<uintptr_t>(module);return CE_OK;};
    uint64_t total=0;bool subtract=false,first=true;
    for(;;){
        uint64_t term=0;int status=operand(term);if(status)return status;
        for(;;){skip();if(at>=s.size()||s[at]!=L'*')break;++at;uint64_t factor=0;status=operand(factor);if(status)return status;
            if(factor&&term>UINT64_MAX/factor)return error(CE_INVALID_ARGUMENT,L"Address expression multiplication overflows.");term*=factor;}
        if(first){total=term;first=false;}
        else if(subtract){if(term>total)return error(CE_INVALID_ARGUMENT,L"Address expression subtracts below zero.");total-=term;}
        else{if(total>UINT64_MAX-term)return error(CE_INVALID_ARGUMENT,L"Address expression addition overflows.");total+=term;}
        skip();if(at>=s.size())break;const wchar_t op=s[at++];
        if(op==L'+')subtract=false;
        else if(op==L'-'){skip();if(at<s.size()&&s[at]==L'-'){++at;subtract=false;}else subtract=true;}
        else return error(CE_INVALID_ARGUMENT,L"Unsupported address expression; use + - * and module names.");}
    if(!valid_range(total,1))return error(CE_INVALID_ARGUMENT,L"Address expression is outside the host address space.");
    address=total;return CE_OK;
}
}
