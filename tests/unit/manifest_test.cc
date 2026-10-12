/*
 * PROJECT : SIFTWING
 * FILE    : manifest_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 独立黄金文本、摘要已知向量、严格词法、统计和预算失败行为
 */
#include "siftwing/snapshot/manifest.hpp"
#include <iostream>
#include <limits>
#include <locale>

namespace {
using namespace siftwing::snapshot;
unsigned checks=0,failures=0;
void check(bool ok,const char *label){++checks;if(!ok){++failures;std::cerr<<label<<'\n';}}
template<class T> void error(const siftwing::base::Result<T> &r,std::string_view context){
    check(!r,"expected failure");if(!r){check(r.error().context==context && !r.error().message.empty(),"error context");}
}
constexpr auto maximum=std::numeric_limits<std::uint64_t>::max();
const ManifestLimits limits{maximum,maximum,maximum};
const std::string xhash="2d711642b726b04401627ca9fbac32f5c8530fb1903cc4db02258717921a4881";
Manifest sample(){
    Manifest m{siftwing::base::SnapshotVersion::from_integer(0).value(),"test-1",{1,1,2,1},
        {std::string(64,'a'),std::string(64,'b')},{2,1,4,1,1},{std::string(64,'c'),std::string(64,'d')},{}};
    for(auto &f:m.files){f={1,1,xhash};}m.files[0].bytes=88;m.files[1].bytes=56;return m;
}
struct Punctuation:std::numpunct<char>{char do_thousands_sep()const override{return '_';}std::string do_grouping()const override{return "\3";}};
struct LocaleGuard{std::locale old=std::locale();~LocaleGuard(){std::locale::global(old);}};
}
int main(){
    try{
        check(sha256_bytes("").value()=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855","empty SHA256 known vector");
        check(sha256_bytes("abc").value()=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","abc known vector");
        check(sha256_bytes(std::string(1000000,'a')).value()=="cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0","million a known vector");
        auto m=sample();const auto bytes=serialize_manifest(m,limits).value();
        // 黄金文本人工列出全部角色/次序，与生产serializer不共享循环或模板。
        const std::string expected="SIFTWING-SNAPSHOT 1\nI 0 test-1\nS 1 1 2 1 1 "+std::string(64,'a')+" "+std::string(64,'b')+
            "\nR 2 1 4 1 1 "+std::string(64,'c')+" "+std::string(64,'d')+
            "\nF documents.bin 1 88 "+xhash+"\nF offsets.bin 1 56 "+xhash+
            "\nF search-lexicon.txt 1 1 "+xhash+"\nF search-weights.txt 1 1 "+xhash+
            "\nF recommendation-dictionary.txt 1 1 "+xhash+"\nF recommendation-characters.txt 1 1 "+xhash+"\n";
        check(bytes==expected,"independent exact golden text");
        check(serialize_manifest(parse_manifest(expected,limits).value(),limits).value()==expected,"owning exact parse golden");
        check(serialize_manifest(m,{static_cast<std::uint64_t>(bytes.size()),88,148}).value()==bytes,"all exact budgets inclusive");
        error(serialize_manifest(m,{static_cast<std::uint64_t>(bytes.size()-1),88,148}),"manifest.bytes");
        error(parse_manifest(bytes,{static_cast<std::uint64_t>(bytes.size()-1),88,148}),"manifest.bytes");
        error(validate_manifest(m,{maximum,87,maximum}),"manifest.file_bytes");
        error(validate_manifest(m,{maximum,maximum,147}),"manifest.total_bytes");
        for(const auto l:std::array<ManifestLimits,3>{{{0,1,1},{1,0,1},{1,1,0}}}){
            error(validate_manifest(m,l),"manifest.limits");error(parse_manifest("",l),"manifest.limits");}
        auto invalid=m;invalid.build_version="../bad";error(validate_manifest(invalid,limits),"manifest.metadata");
        invalid=m;invalid.build_version=std::string(129,'a');error(validate_manifest(invalid,limits),"manifest.metadata");
        invalid=m;invalid.build_version=std::string(128,'a');check(static_cast<bool>(validate_manifest(invalid,limits)),"build identity endpoint");
        for(std::size_t i=0;i<10;++i){invalid=m;
            const std::array<std::string*,10> hashes{{&invalid.search_provenance.input_sha256,&invalid.search_provenance.config_sha256,
                &invalid.recommendation_provenance.input_sha256,&invalid.recommendation_provenance.config_sha256,
                &invalid.files[0].sha256,&invalid.files[1].sha256,&invalid.files[2].sha256,&invalid.files[3].sha256,&invalid.files[4].sha256,&invalid.files[5].sha256}};
            *hashes[i]=std::string(64,'A');error(validate_manifest(invalid,limits),"manifest.metadata");}
        for(std::size_t i=0;i<6;++i){invalid=m;invalid.files[i].format_version=2;error(validate_manifest(invalid,limits),"manifest.version");}
        invalid=m;invalid.search.terms=2;error(validate_manifest(invalid,limits),"manifest.statistics");
        invalid=m;invalid.search.postings=3;error(validate_manifest(invalid,limits),"manifest.statistics");
        invalid=m;invalid.search.documents=0;error(validate_manifest(invalid,limits),"manifest.statistics");
        invalid=m;invalid.recommendation.documents=0;error(validate_manifest(invalid,limits),"manifest.statistics");
        invalid=m;invalid.recommendation.characters=0;error(validate_manifest(invalid,limits),"manifest.statistics");
        invalid=m;invalid.recommendation.character_links=0;error(validate_manifest(invalid,limits),"manifest.statistics");
        invalid=m;invalid.search.documents=maximum;error(validate_manifest(invalid,limits),"manifest.overflow");
        invalid=m;invalid.files[0].bytes=maximum;error(validate_manifest(invalid,limits),"manifest.overflow");
        for(std::size_t i=0;i<6;++i){invalid=m;invalid.files[i].bytes=0;error(validate_manifest(invalid,limits),"manifest.length");}
        auto empty=m;empty.search={};empty.recommendation={};empty.files[0].bytes=0;empty.files[1].bytes=32;
        check(static_cast<bool>(parse_manifest(serialize_manifest(empty,limits).value(),limits)),"empty complete six artifact metadata");
        empty.search.documents=2;empty.files[0].bytes=176;empty.files[1].bytes=80;
        check(static_cast<bool>(validate_manifest(empty,limits)),"all empty selected documents still count N");
        auto full=m;full.snapshot=siftwing::base::SnapshotVersion::from_integer(maximum).value();full.search.total_tokens=maximum;
        full.recommendation.documents=maximum;full.recommendation.total_tokens=maximum;
        check(serialize_manifest(parse_manifest(serialize_manifest(full,limits).value(),limits).value(),limits).value()==serialize_manifest(full,limits).value(),"full width values not rounded");
        for(std::size_t n=0;n<bytes.size();++n){check(!parse_manifest(std::string_view(bytes).substr(0,n),limits),"all truncated prefixes rejected");}
        for(const auto &suffix:std::array<std::string,4>{{"\n","F extra 1 1 "+xhash+"\n"," ",std::string(1,'\0')}}){check(!parse_manifest(bytes+suffix,limits),"trailing rejected");}
        for(const auto &token:std::array<std::string,9>{{"00","+0","-0"," 0","0 ","0\t","18446744073709551616","1e3",std::string("0\0",2)}}){
            auto bad=bytes;bad.replace(bad.find("I 0")+2,1,token);error(parse_manifest(bad,limits),"manifest.syntax");}
        auto bad=bytes;bad.replace(bad.find("SNAPSHOT 1")+9,1,"2");error(parse_manifest(bad,limits),"manifest.version");
        bad=bytes;bad.replace(bad.find("F offsets.bin"),13,"F ../evil.bin");error(parse_manifest(bad,limits),"manifest.syntax");
        bad=bytes;bad.insert(bad.find('\n'),1,'\r');error(parse_manifest(bad,limits),"manifest.syntax");
        // 以下伪造payload只证明字节身份，不冒充SWDR/SWDI格式解码或跨文件语义验证。
        const std::array<std::string,6> payloads{{std::string(88,'a'),std::string(56,'b'),"x","x","x","x"}};
        for(std::size_t i=0;i<6;++i){m.files[i].sha256=sha256_bytes(payloads[i]).value();}
        for(std::size_t i=0;i<6;++i){const auto role=static_cast<Artifact>(i);check(static_cast<bool>(verify_artifact(m,role,payloads[i],limits)),"six exact payload identities");
            auto changed=payloads[i];changed[0]='z';error(verify_artifact(m,role,changed,limits),"manifest.checksum");
            error(verify_artifact(m,role,payloads[i]+"x",limits),"manifest.length");}
        error(verify_artifact(m,static_cast<Artifact>(6),"",limits),"manifest.role");
        const auto marker=completion_marker(bytes,limits).value();
        check(marker=="SIFTWING-COMPLETE 1 0 "+sha256_bytes(bytes).value()+"\n","completion exact bound bytes");
        check(static_cast<bool>(verify_completion(bytes,marker,limits)),"completion accepted");
        error(verify_completion(bytes,marker+"\n",limits),"manifest.completion");
        bad=bytes;bad.replace(bad.find("test-1"),6,"test-2");error(verify_completion(bad,marker,limits),"manifest.completion");
        error(completion_marker("invalid",limits),"manifest.syntax");
        {LocaleGuard guard;std::locale::global(std::locale(guard.old,new Punctuation));check(serialize_manifest(sample(),limits).value()==expected,"locale independent encoding");}
        auto owned=parse_manifest(std::string(bytes),limits).value();auto copy=owned;copy.build_version="other";
        check(owned.build_version=="test-1","decoded values own copied source");
        auto moved=std::move(owned);check(serialize_manifest(moved,limits).value()==bytes,"ordinary value move transfers complete metadata");
    }catch(const std::exception &e){++failures;std::cerr<<e.what()<<'\n';}
    std::cout<<"checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;
}
