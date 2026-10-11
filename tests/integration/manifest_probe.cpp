/*
 * PROJECT : SIFTWING
 * FILE    : manifest_probe.cpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 向独立Python参考暴露内存哈希及manifest规范化/完成标记
 */
#include "siftwing/snapshot/manifest.h"
#include <iostream>
#include <iterator>

int main(int argc,char **argv){
    try{
        using namespace siftwing::snapshot;
        if(argc!=2){return 2;}
        const std::string mode=argv[1];const std::string bytes{std::istreambuf_iterator<char>(std::cin),{}};
        const ManifestLimits limits{65536,UINT64_MAX,UINT64_MAX};
        if(mode=="hash"){std::cout<<sha256_bytes(bytes).value()<<'\n';return 0;}
        const auto parsed=parse_manifest(bytes,limits);
        if(!parsed){std::cerr<<parsed.error().context<<'\n';return 1;}
        if(mode=="parse"){std::cout<<serialize_manifest(parsed.value(),limits).value();return 0;}
        if(mode=="complete"){std::cout<<completion_marker(bytes,limits).value();return 0;}
        return 2;
    }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
