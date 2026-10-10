# cppjieba5.6.7的Siftwing内存构造补丁；保留原路径API，不修改分词算法。
# 只复制并修改构建目录内头文件。原始哈希不符即配置失败，防止升级静默错配。
function(siftwing_prepare_cppjieba source destination)
    file(SHA256 "${source}/include/cppjieba/DictTrie.hpp" dict_hash)
    file(SHA256 "${source}/include/cppjieba/HMMModel.hpp" hmm_hash)
    if(NOT dict_hash STREQUAL "88908f2ccb24738d8eb121733b3422c68579110f79ce1f81112d4c99c2bc67c8" OR NOT hmm_hash STREQUAL "bf0f8d5b93d3d4b54b87c5010a0e797fbe3fb1ec43057be3c8e5b6a875a796cc")
        message(FATAL_ERROR "cppjieba memory patch requires the verified 5.6.7 headers")
    endif()
    file(MAKE_DIRECTORY "${destination}")
    file(COPY "${source}/include/cppjieba" DESTINATION "${destination}")
    file(READ "${destination}/cppjieba/DictTrie.hpp" dictionary)
    set(marker "  ~DictTrie() {")
    set(memory_constructor [=[
  // Siftwing修改：只接收已验证的拥有型记录，不读路径或使用全局词典缓存。
  DictTrie(std::vector<DictUnit> main, std::vector<DictUnit> user) {
    freq_sum_ = CalcFreqSum(main);
    CalculateWeight(main, freq_sum_);
    std::vector<DictUnit> sorted = main;
    std::sort(sorted.begin(), sorted.end(), WeightCompare);
    min_weight_ = sorted.front().weight;
    max_weight_ = sorted.back().weight;
    median_weight_ = sorted[sorted.size() / 2].weight;
    user_word_default_weight_ = median_weight_;
    base_static_node_infos_ = std::make_shared<const std::vector<DictUnit> >(std::move(main));
    for (auto& word : user) {
      word.weight = word.weight == 0.0 ? median_weight_ : log(word.weight / freq_sum_);
      if (word.word.size() == 1) { user_dict_single_chinese_word_.insert(word.word[0]); }
    }
    static_node_infos_ = std::move(user);
    CreateTrie();
  }

]=])
    string(REPLACE "${marker}" "${memory_constructor}${marker}" dictionary "${dictionary}")
    file(WRITE "${destination}/cppjieba/DictTrie.hpp" "${dictionary}")
    file(READ "${destination}/cppjieba/HMMModel.hpp" model)
    set(old "  HMMModel(const string& modelPath) {")
    set(new "  // Siftwing修改：默认初始化后由适配器填充已验证内存参数。\n  HMMModel() {")
    string(REPLACE "${old}" "${new}" model "${model}")
    set(old "    LoadModel(modelPath);\n  }")
    set(new "  }\n  HMMModel(const string& modelPath) : HMMModel() {\n    LoadModel(modelPath);\n  }")
    string(REPLACE "${old}" "${new}" model "${model}")
    file(WRITE "${destination}/cppjieba/HMMModel.hpp" "${model}")
endfunction()
