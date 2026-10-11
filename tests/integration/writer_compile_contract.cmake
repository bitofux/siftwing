if(NOT DEFINED CXX OR NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR)
    message(FATAL_ERROR "CXX, SOURCE_DIR and BINARY_DIR are required")
endif()
file(MAKE_DIRECTORY "${BINARY_DIR}")
function(check_compile name should_pass diagnostic code)
    set(source "${BINARY_DIR}/${name}.cpp")
    file(WRITE "${source}" "#include <siftwing/snapshot/writer.h>\n#include <type_traits>\n${code}\n")
    execute_process(COMMAND "${CXX}" -std=c++17 -Wall -Wextra -Wpedantic -Wconversion -Wshadow
        -Werror=unused-result -fsyntax-only "-I${SOURCE_DIR}/include" "${source}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    file(WRITE "${BINARY_DIR}/${name}.txt" "exit=${result}\n${output}${error}")
    if(should_pass)
        if(NOT result STREQUAL "0")
            message(FATAL_ERROR "${name} must compile: ${error}")
        endif()
    elseif(result STREQUAL "0" OR NOT error MATCHES "${diagnostic}")
        message(FATAL_ERROR "${name} expected rejection: ${result}\n${error}")
    endif()
endfunction()
# 独立拥有发布结果可复制；借用函数不接管不可复制索引；强版本/部件顺序和显式结果受检。
check_compile(valid TRUE "" [=[
using namespace siftwing::snapshot;
static_assert(std::is_copy_constructible_v<PublishedSnapshot>);
static_assert(std::is_copy_assignable_v<PublishedSnapshot>);
static_assert(!std::is_default_constructible_v<SnapshotMetadata>);
static_assert(std::is_nothrow_move_constructible_v<siftwing::base::Result<PublishedSnapshot>>);
void f(const siftwing::index::WeightedIndex& s,const siftwing::index::RecommendationDictionary& r,
       const SnapshotMetadata& m){auto result=write_snapshot("output",{},s,r,m,{});(void)result;}
]=])
check_compile(wrong_identity FALSE "(convert|conversion|initialize)" [=[
void f(siftwing::snapshot::SnapshotMetadata& m,siftwing::base::DocumentId d){m.snapshot=d;}
]=])
check_compile(raw_identity FALSE "(convert|conversion|initialize)" [=[
void f(siftwing::snapshot::SnapshotMetadata& m){m.snapshot=7;}
]=])
check_compile(wrong_parts FALSE "(invalid|incompatible|reference|convert|conversion)" [=[
void f(const siftwing::index::WeightedIndex& s,const siftwing::index::RecommendationDictionary& r,
       const siftwing::snapshot::SnapshotMetadata& m){auto result=siftwing::snapshot::write_snapshot("output",{},r,s,m,{});(void)result;}
]=])
check_compile(discard FALSE "(nodiscard|unused-result|ignoring return)" [=[
void f(const siftwing::index::WeightedIndex& s,const siftwing::index::RecommendationDictionary& r,
       const siftwing::snapshot::SnapshotMetadata& m){siftwing::snapshot::write_snapshot("output",{},s,r,m,{});}
]=])
check_compile(no_implicit_durability FALSE "(convert|conversion|initialize)" [=[
void f(){siftwing::snapshot::PublicationDurability d=0;(void)d;}
]=])
message(STATUS "6 writer compile contract cases passed")
