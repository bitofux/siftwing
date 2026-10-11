/*
 * PROJECT : SIFTWING
 * FILE    : manifest.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 规范manifest词法/语义与固定文件字节校验
 * IMPLEMENTATION : 固定十行避免任意路径，SHA-256按标准无符号模运算处理完整字节
 */
#include "siftwing/snapshot/manifest.h"
#include <algorithm>
#include <charconv>
#include <limits>
#include <vector>

namespace siftwing::snapshot {
namespace {
template<class T> base::Result<T> fail(const char *context, const char *message) {
    return base::Result<T>::failure({message,context});
}
bool configured(const ManifestLimits &l) { return l.max_manifest_bytes && l.max_file_bytes && l.max_total_file_bytes; }
bool digest(std::string_view s) {
    return s.size()==64 && std::all_of(s.begin(),s.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});
}
bool build_name(std::string_view s) {
    return !s.empty() && s.size()<=128 && std::all_of(s.begin(),s.end(),[](char c){
        return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='+'||c=='-';});
}
std::string decimal(std::uint64_t value) {
    std::array<char,20> b{}; const auto r=std::to_chars(b.data(),b.data()+b.size(),value);
    return {b.data(),static_cast<std::size_t>(r.ptr-b.data())};
}
bool number(std::string_view s,std::uint64_t &out) {
    if(s.empty() || (s.size()>1 && s.front()=='0')) {return false;}
    if(!std::all_of(s.begin(),s.end(),[](char c){return c>='0'&&c<='9';})) {return false;}
    const auto r=std::from_chars(s.data(),s.data()+s.size(),out);
    return r.ec==std::errc{} && r.ptr==s.data()+s.size();
}
// 只在固定十行内拆字段；任何多行/超字段先拒绝，不按攻击者声明分配容器。
bool line(std::string_view &input,std::vector<std::string_view> &parts,std::size_t count) {
    const auto end=input.find('\n'); if(end==std::string_view::npos){return false;}
    auto s=input.substr(0,end); input.remove_prefix(end+1); parts.clear();
    while(true){
        const auto space=s.find(' '); auto p=s.substr(0,space);
        if(p.empty() || p.find_first_of("\r\t\0",0,3)!=std::string_view::npos || parts.size()==count){return false;}
        parts.push_back(p); if(space==std::string_view::npos){break;} s.remove_prefix(space+1);
    }
    return parts.size()==count;
}
// SHA-256的右旋与所有加法都在uint32无符号值域，回绕是标准算法定义。
std::uint32_t rotate(std::uint32_t v,unsigned n) {return (v>>n)|(v<<(32-n));}
void compress(std::array<std::uint32_t,8> &h,const std::array<unsigned char,64> &block) {
    constexpr std::array<std::uint32_t,64> k{{
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2}};
    std::array<std::uint32_t,64> w{};
    for(std::size_t i=0;i<16;++i){for(std::size_t j=0;j<4;++j){w[i]=(w[i]<<8)|block[4*i+j];}}
    for(std::size_t i=16;i<64;++i){
        const auto x=w[i-15],y=w[i-2];
        w[i]=w[i-16]+(rotate(x,7)^rotate(x,18)^(x>>3))+w[i-7]+(rotate(y,17)^rotate(y,19)^(y>>10));
    }
    auto a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],v=h[7];
    for(std::size_t i=0;i<64;++i){
        const auto t1=v+(rotate(e,6)^rotate(e,11)^rotate(e,25))+((e&f)^((~e)&g))+k[i]+w[i];
        const auto t2=(rotate(a,2)^rotate(a,13)^rotate(a,22))+((a&b)^(a&c)^(b&c));
        v=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=v;
}
} // namespace

base::Result<void> validate_manifest(const Manifest &m,const ManifestLimits &l) {
    constexpr auto max=std::numeric_limits<std::uint64_t>::max();
    if(!configured(l)){return fail<void>("manifest.limits","three positive limits required");}
    if(!build_name(m.build_version) || !digest(m.search_provenance.input_sha256) ||
        !digest(m.search_provenance.config_sha256) || !digest(m.recommendation_provenance.input_sha256) ||
        !digest(m.recommendation_provenance.config_sha256)) {return fail<void>("manifest.metadata","invalid build identity or provenance digest");}
    const auto &s=m.search; const auto &r=m.recommendation;
    if(s.terms>s.postings || s.postings>s.total_tokens || (s.documents==0 && s.total_tokens!=0) ||
        ((s.terms==0)!=(s.total_tokens==0)) || ((s.postings==0)!=(s.total_tokens==0)) ||
        (s.terms && s.documents<=max/s.terms && s.postings>s.documents*s.terms) ||
        r.terms>r.total_tokens || (r.documents==0 && r.total_tokens!=0) ||
        ((r.terms==0)!=(r.total_tokens==0)) || ((r.characters==0)!=(r.terms==0)) ||
        ((r.character_links==0)!=(r.terms==0)) || r.character_links<r.terms || r.characters>r.character_links) {
        return fail<void>("manifest.statistics","inconsistent independent search or recommendation statistics");
    }
    if(s.documents>(max-32)/24 || s.documents>max/88){return fail<void>("manifest.overflow","document encoding size cannot fit uint64");}
    std::uint64_t total=0;
    for(std::size_t i=0;i<m.files.size();++i){
        const auto &f=m.files[i];
        if(f.format_version!=artifact_layout[i].version){return fail<void>("manifest.version","unsupported artifact format");}
        if(!digest(f.sha256)){return fail<void>("manifest.metadata","invalid artifact digest");}
        if((i==0 && ((s.documents==0 && f.bytes!=0) || f.bytes<88*s.documents)) ||
            (i==1 && f.bytes!=32+24*s.documents) || (i>1 && f.bytes==0)) {
            return fail<void>("manifest.length","artifact size inconsistent with document layout or empty encoding");
        }
        if(f.bytes>l.max_file_bytes){return fail<void>("manifest.file_bytes","artifact budget exceeded");}
        if(f.bytes>max-total){return fail<void>("manifest.overflow","total artifact bytes overflow");}
        total+=f.bytes;
        if(total>l.max_total_file_bytes){return fail<void>("manifest.total_bytes","total artifact budget exceeded");}
    }
    return base::Result<void>::success();
}
base::Result<std::string> serialize_manifest(const Manifest &m,const ManifestLimits &l) {
    const auto valid=validate_manifest(m,l); if(!valid){return base::Result<std::string>::failure(valid.error());}
    std::string out; bool ok=true;
    const auto append=[&](std::string_view s){if(!ok || s.size()>l.max_manifest_bytes-out.size()){ok=false;}else{out.append(s);}};
    append("SIFTWING-SNAPSHOT 1\nI ");append(decimal(m.snapshot.value()));append(" ");append(m.build_version);append("\nS ");
    for(auto n:std::array<std::uint64_t,4>{m.search.documents,m.search.terms,m.search.total_tokens,m.search.postings}){append(decimal(n));append(" ");}
    append("1 ");append(m.search_provenance.input_sha256);append(" ");append(m.search_provenance.config_sha256);append("\nR ");
    for(auto n:std::array<std::uint64_t,5>{m.recommendation.documents,m.recommendation.terms,m.recommendation.total_tokens,m.recommendation.characters,m.recommendation.character_links}){append(decimal(n));append(" ");}
    append(m.recommendation_provenance.input_sha256);append(" ");append(m.recommendation_provenance.config_sha256);append("\n");
    for(std::size_t i=0;i<m.files.size();++i){append("F ");append(artifact_layout[i].name);append(" ");append(decimal(m.files[i].format_version));append(" ");append(decimal(m.files[i].bytes));append(" ");append(m.files[i].sha256);append("\n");}
    if(!ok){return fail<std::string>("manifest.bytes","manifest output budget exceeded");}
    return base::Result<std::string>::success(std::move(out));
}
base::Result<Manifest> parse_manifest(std::string_view bytes,const ManifestLimits &l) {
    if(!configured(l)){return fail<Manifest>("manifest.limits","three positive limits required");}
    if(bytes.size()>l.max_manifest_bytes){return fail<Manifest>("manifest.bytes","manifest input budget exceeded");}
    std::vector<std::string_view> p; p.reserve(8); std::uint64_t n=0;
    const auto syntax=[](){return fail<Manifest>("manifest.syntax","require one complete canonical manifest");};
    if(!line(bytes,p,2) || p[0]!="SIFTWING-SNAPSHOT" || !number(p[1],n)){return syntax();}
    if(n!=manifest_format_version){return fail<Manifest>("manifest.version","unsupported manifest format");}
    if(!line(bytes,p,3) || p[0]!="I" || !number(p[1],n)){return syntax();}
    if(!build_name(p[2])){return fail<Manifest>("manifest.metadata","invalid build identity");}
    Manifest m{base::SnapshotVersion::from_integer(n).value(),std::string(p[2]),{},{},{},{},{}};
    if(!line(bytes,p,8) || p[0]!="S" || !number(p[1],m.search.documents) || !number(p[2],m.search.terms) ||
        !number(p[3],m.search.total_tokens) || !number(p[4],m.search.postings) || !number(p[5],n)){return syntax();}
    if(n!=1){return fail<Manifest>("manifest.version","unsupported weight policy");}
    if(!digest(p[6]) || !digest(p[7])){return fail<Manifest>("manifest.metadata","invalid search provenance digest");}
    m.search_provenance={std::string(p[6]),std::string(p[7])};
    if(!line(bytes,p,8) || p[0]!="R" || !number(p[1],m.recommendation.documents) || !number(p[2],m.recommendation.terms) ||
        !number(p[3],m.recommendation.total_tokens) || !number(p[4],m.recommendation.characters) || !number(p[5],m.recommendation.character_links)){return syntax();}
    if(!digest(p[6]) || !digest(p[7])){return fail<Manifest>("manifest.metadata","invalid recommendation provenance digest");}
    m.recommendation_provenance={std::string(p[6]),std::string(p[7])};
    for(std::size_t i=0;i<m.files.size();++i){
        if(!line(bytes,p,5) || p[0]!="F" || p[1]!=artifact_layout[i].name || !number(p[2],n) || !number(p[3],m.files[i].bytes)){return syntax();}
        if(n!=artifact_layout[i].version){return fail<Manifest>("manifest.version","unsupported artifact format");}
        if(!digest(p[4])){return fail<Manifest>("manifest.metadata","invalid artifact digest");}
        m.files[i].format_version=static_cast<std::uint32_t>(n); m.files[i].sha256=std::string(p[4]);
    }
    if(!bytes.empty()){return syntax();}
    const auto valid=validate_manifest(m,l); if(!valid){return base::Result<Manifest>::failure(valid.error());}
    return base::Result<Manifest>::success(std::move(m));
}
base::Result<std::string> sha256_bytes(std::string_view bytes) {
    if(bytes.size()>std::numeric_limits<std::uint64_t>::max()/8){return fail<std::string>("manifest.hash_length","SHA256 bit length cannot fit uint64");}
    std::array<std::uint32_t,8> h{{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}};
    const auto bit_length=static_cast<std::uint64_t>(bytes.size())*8;
    std::array<unsigned char,64> block{};
    while(bytes.size()>=64){for(std::size_t i=0;i<64;++i){block[i]=static_cast<unsigned char>(bytes[i]);}compress(h,block);bytes.remove_prefix(64);}
    block.fill(0); for(std::size_t i=0;i<bytes.size();++i){block[i]=static_cast<unsigned char>(bytes[i]);}block[bytes.size()]=0x80;
    if(bytes.size()>=56){compress(h,block);block.fill(0);}
    for(std::size_t i=0;i<8;++i){block[63-i]=static_cast<unsigned char>(bit_length>>(8*i));}compress(h,block);
    constexpr char hex[]="0123456789abcdef"; std::string result(64,'0');
    for(std::size_t i=0;i<h.size();++i){for(std::size_t j=0;j<8;++j){result[8*i+j]=hex[(h[i]>>((7-j)*4))&15U];}}
    return base::Result<std::string>::success(std::move(result));
}
base::Result<void> verify_artifact(const Manifest &m,Artifact role,std::string_view bytes,const ManifestLimits &l) {
    const auto valid=validate_manifest(m,l);if(!valid){return base::Result<void>::failure(valid.error());}
    const auto i=static_cast<std::size_t>(role);if(i>=m.files.size()){return fail<void>("manifest.role","unknown artifact role");}
    if(bytes.size()!=m.files[i].bytes){return fail<void>("manifest.length","payload byte length differs");}
    const auto hash=sha256_bytes(bytes);if(!hash){return base::Result<void>::failure(hash.error());}
    if(hash.value()!=m.files[i].sha256){return fail<void>("manifest.checksum","payload SHA256 differs");}
    return base::Result<void>::success();
}
base::Result<std::string> completion_marker(std::string_view bytes,const ManifestLimits &l) {
    const auto m=parse_manifest(bytes,l);if(!m){return base::Result<std::string>::failure(m.error());}
    const auto hash=sha256_bytes(bytes);if(!hash){return base::Result<std::string>::failure(hash.error());}
    return base::Result<std::string>::success("SIFTWING-COMPLETE 1 "+decimal(m.value().snapshot.value())+" "+hash.value()+"\n");
}
base::Result<void> verify_completion(std::string_view bytes,std::string_view marker,const ManifestLimits &l) {
    const auto expected=completion_marker(bytes,l);if(!expected){return base::Result<void>::failure(expected.error());}
    if(marker!=expected.value()){return fail<void>("manifest.completion","require exact marker bound to manifest bytes");}
    return base::Result<void>::success();
}
} // namespace siftwing::snapshot
