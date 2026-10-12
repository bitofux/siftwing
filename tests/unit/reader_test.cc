/*
 * PROJECT : SIFTWING
 * FILE    : reader_test.cc
 * AUTHOR  : bitofux
 * DATE    : 2026-10-11
 * BRIEF   : 证明Writer接线、移动生命、短IO与所有文件句柄失败释放合同
 */
#include "siftwing/snapshot/reader.hpp"
#include "siftwing/snapshot/writer.hpp"
#include <array>
#include <cerrno>
#include <cstdarg>
#include <cfenv>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <new>
#include <type_traits>
#include <utility>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
using namespace siftwing;
namespace {
bool active=false,triggered=false; std::string operation,behavior; int target=1;
std::map<std::string,int> counts; int checks=0,failures=0;
bool inject(const char *name) {
    if (!active) { return false; } const int call=++counts[name];
    if (!triggered && operation==name && call==target) { triggered=true; errno=EIO; return true; } return false;
}
void check(bool value,const char *message) { ++checks; if (!value) { ++failures; std::cerr<<message<<'\n'; } }
std::size_t descriptors() { std::size_t n=0; for (const auto &entry:std::filesystem::directory_iterator("/proc/self/fd")) { (void)entry; ++n; } return n; }
base::DocumentId doc(std::uint64_t value) { return base::DocumentId::from_integer(value).value(); }
base::SnapshotVersion version(std::uint64_t value) { return base::SnapshotVersion::from_integer(value).value(); }
snapshot::SnapshotReadLimits budgets() { return {{{10000,100000},100,1000000,100000}, {1000000,1000000,1000000},
    {{1000,10000,1000,100,10000},100,100000,1000000,100},{{1000,10000,1000,100,10000},100,1000000,1000,10000}}; }
}
extern "C" {
int __real_openat(int,const char *,int,...);
ssize_t __real_pread(int,void *,size_t,off_t);
int __real_fstat(int,struct stat *);
int __real_fstatat(int,const char *,struct stat *,int);
int __real_close(int);
DIR *__real_fdopendir(int);
struct dirent *__real_readdir(DIR *);
int __real_closedir(DIR *);
int __wrap_openat(int dir,const char *path,int flags,...) {
    int mode=0;
    if (flags&O_CREAT) { va_list args; va_start(args,flags); mode=va_arg(args,int); va_end(args); }
    if (inject("openat")) { if (behavior=="eintr") { errno=EINTR; } return -1; }
    return __real_openat(dir,path,flags,mode);
}
ssize_t __wrap_pread(int fd,void *out,size_t count,off_t offset) {
    if (inject("pread")) { if (behavior=="eintr") { errno=EINTR; return -1; }
        if (behavior=="exception") { throw std::bad_alloc{}; }
        if (behavior=="short") { return __real_pread(fd,out,1,offset); }
        if (behavior=="eof") { return 0; } return -1; }
    return __real_pread(fd,out,count,offset);
}
int __wrap_fstat(int fd,struct stat *out) {
    if (inject("fstat")) { if (behavior=="changed") { const int r=__real_fstat(fd,out); if (r==0) { ++out->st_size; } return r; }
        if (behavior=="eintr") { errno=EINTR; } return -1; }
    return __real_fstat(fd,out);
}
int __wrap_fstatat(int dir,const char *path,struct stat *out,int flags) {
    if (inject("fstatat")) { if (behavior=="eintr") { errno=EINTR; } return -1; }
    return __real_fstatat(dir,path,out,flags);
}
int __wrap_close(int fd) { if (inject("close")) { (void)__real_close(fd); errno=EIO; return -1; } return __real_close(fd); }
DIR *__wrap_fdopendir(int fd) { if (inject("fdopendir")) { return nullptr; } return __real_fdopendir(fd); }
struct dirent *__wrap_readdir(DIR *dir) { if (inject("readdir")) { return nullptr; } return __real_readdir(dir); }
int __wrap_closedir(DIR *dir) { if (inject("closedir")) { (void)__real_closedir(dir); errno=EIO; return -1; } return __real_closedir(dir); }
}
int main(int argc,char **argv) {
    static_assert(!std::is_copy_constructible_v<snapshot::SnapshotReader> && std::is_nothrow_move_constructible_v<snapshot::SnapshotReader>);
    static_assert(std::is_nothrow_move_assignable_v<snapshot::SnapshotReader>);
    if (argc!=2) { return 2; } std::string pattern=std::string(argv[1])+"/reader-unit-XXXXXX";
    char *created=::mkdtemp(pattern.data()); if (!created) { return 2; } const std::filesystem::path parent{created};
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path,ec); } } cleanup{parent};
    const auto maximum=std::numeric_limits<std::uint64_t>::max();
    std::vector<document::DocumentRecord> records{{doc(maximum),"标题",std::string{"a\0b",3},
        {document::SourceKind::rss,{"rss",maximum},std::string{},std::nullopt,std::string{"日"}}},
        {doc(0),"empty","",{document::SourceKind::txt,{"",0},std::nullopt,std::string{},std::nullopt}},
        {doc(77),"third","正文",{}}};
    std::vector<index::IndexDocument> inputs{{doc(0),{{},0}},{doc(77),{{{"a",3},{"中",4}},7}},
        {doc(maximum),{{{"a",2},{"😀",1}},3}}}; auto limits=budgets();
    auto raw=index::InvertedIndex::build(inputs,limits.search); if (!raw) { return 2; }
    auto weighted=index::WeightedIndex::build(std::move(raw).value(),{100,100,1000}); if (!weighted) { return 2; }
    auto recommendation=index::RecommendationDictionary::build({{{"abba",7},{"😀中",9}},16,5},limits.recommendation); if (!recommendation) { return 2; }
    auto saved=snapshot::write_snapshot(parent.string(),records,weighted.value(),recommendation.value(),
        {version(0),"test-v1",{std::string(64,'a'),std::string(64,'b')},{std::string(64,'c'),std::string(64,'d')}},
        {limits.documents,limits.manifest}); if (!saved) { return 2; }
    const auto fd_before=descriptors(); operation="none"; active=true;
    auto loaded=snapshot::SnapshotReader::open(parent.string(),version(0),limits); active=false;
    check(static_cast<bool>(loaded),"Writer产物应成功加载"); if (!loaded) { std::cerr<<loaded.error().context; return 1; }
    const auto observed=counts; check(descriptors()==fd_before,"成功后不保存fd");
    auto &reader=loaded.value(); check(reader.manifest()->snapshot==version(0),"完整身份");
    check(reader.documents()->size()==3 && reader.documents()->front().doc_id==doc(0),"排序记录");
    check(reader.document(doc(42))==nullptr,"未知ID为空");
    for (const auto &original:records) {
        auto expected=document::encode_record(original,limits.documents.record);
        const auto *actual=reader.document(original.doc_id); check(actual!=nullptr,"每个身份可查");
        if (actual) { auto bytes=document::encode_record(*actual,limits.documents.record); check(bytes && expected && bytes.value()==expected.value(),"任意字节及可选字段保持"); }
    }
    auto exported=reader.search()->serialize({1000000}); auto expected=weighted.value().serialize({1000000});
    check(exported && expected && exported.value()==expected.value(),"所有搜索统计与权重保持");
    check(reader.recommendation()->characters()==recommendation.value().characters(),"独立推荐关系保持");
    auto moved=std::move(reader); check(reader.manifest()==nullptr && reader.search()==nullptr && reader.documents()==nullptr &&
        reader.recommendation()==nullptr && reader.document(doc(0))==nullptr,"移动源空");
    auto &alias=moved; moved=std::move(alias); check(moved.document(doc(maximum))!=nullptr,"自移动保持");
    auto second=snapshot::SnapshotReader::open(parent.string(),version(0),limits); check(static_cast<bool>(second),"再次加载");
    if (second) { second.value()=std::move(moved); check(moved.manifest()==nullptr && second.value().document(doc(77))!=nullptr,"移动赋值转交"); }
    // 每个成功路径系统调用点都注入一次错误；fd基线独立观测，而非依赖实现计数。
    for (const auto &[name,total]:observed) {
        for (int call=1;call<=total;++call) {
            operation=name; behavior="error"; target=call; triggered=false; counts.clear(); active=true;
            auto rejected=snapshot::SnapshotReader::open(parent.string(),version(0),limits); active=false;
            check(triggered && !rejected,"每个IO/目录/关闭错误都拒绝完整加载"); check(descriptors()==fd_before,"失败后无fd泄漏");
        }
    }
    for (const auto &mode:{"eintr","short","eof","changed"}) {
        behavior=mode; operation=behavior=="changed" ? "fstat" : "pread"; target=behavior=="changed" ? 2 : 1;
        triggered=false; counts.clear(); active=true; auto result=snapshot::SnapshotReader::open(parent.string(),version(0),limits); active=false;
        check(triggered && (static_cast<bool>(result)==(behavior=="eintr" || behavior=="short")),"短读/EINTR重试、EOF/观察到尺寸变化拒绝");
        check(descriptors()==fd_before,"特殊IO无fd泄漏");
    }
    for (const auto &name:{"openat","fstat","fstatat"}) {
        operation=name; behavior="eintr"; target=1; triggered=false; counts.clear(); active=true;
        auto result=snapshot::SnapshotReader::open(parent.string(),version(0),limits); active=false;
        check(triggered && result,"目录打开/尺寸/目录项EINTR重试"); check(descriptors()==fd_before,"重试后无fd泄漏");
    }
    operation="pread"; behavior="exception"; target=1; triggered=false; counts.clear(); bool escaped=false; active=true;
    try { auto result=snapshot::SnapshotReader::open(parent.string(),version(0),limits); (void)result; }
    catch (const std::bad_alloc &) { escaped=true; }
    active=false; check(triggered && escaped,"未知分配异常向上传递"); check(descriptors()==fd_before,"异常释放局部fd");
    for (const auto &path:{std::string{},std::string{"a\0b",3},parent.string()+"/.."}) {
        auto invalid=snapshot::SnapshotReader::open(path,version(0),limits); check(!invalid && invalid.error().context=="reader.path","路径词法失败");
    }
    auto zero=limits; zero.manifest.max_file_bytes=0;
    auto priority=snapshot::SnapshotReader::open("",version(0),zero); check(!priority && priority.error().context=="reader.limits","配置优先于路径");
    const int old_round=std::fegetround(); if (std::fesetround(FE_DOWNWARD)!=0) { return 2; }
    auto rounding=snapshot::SnapshotReader::open(parent.string(),version(0),limits); (void)std::fesetround(old_round);
    check(!rounding && rounding.error().context=="weights.rounding","不兼容数值运行环境拒绝");
    // 成功对象完整拥有，删除本测试自建产物不会改变其记录/权重；不触及用户目录。
    std::filesystem::remove_all(parent/"0"); check(second && second.value().document(doc(maximum))->content==std::string("a\0b",3),"加载后独立于文件生命");
    std::cout<<"reader unit checks="<<checks<<" failures="<<failures<<'\n'; return failures ? 1 : 0;
}
