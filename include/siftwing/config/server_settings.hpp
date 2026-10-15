/*
 * PROJECT : SIFTWING
 * FILE    : server_settings.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-15
 * BRIEF   : 启动线程拥有的服务配置与严格文件/命令行准入
 */
#pragma once
#include "siftwing/base/result.hpp"
#include "siftwing/net/online_server.hpp"
#include "siftwing/logging/logger.hpp"
#include <string>
#include <vector>

namespace siftwing::config {
/**
 * @brief 完整拥有启动配置；默认值沿现有服务，不在内部保存指向自身字符串的view
 * @note 普通值可copy/move；调用者不得并发修改。修改字段后不再具有加载器的验证保证，
 *     底层服务仍会独立验证。没有热更新、环境变量插值或文件监视。
 */
struct ServerSettings final {
    logging::Level log_level = logging::Level::info; ///< 默认info；空file复制stderr，可显式off。
    std::string log_file; ///< 新建0600文件族；启用大小轮转，不追加旧文件或创建父目录。
    logging::Rotation log_rotation; ///< 启动时复制给Logger，stderr不使用轮转参数。
    query::SearchServiceConfig service; ///< 独占路径/查询资源及缓存预算；版本必须显式提供。
    net::OnlineServerConfig server; ///< 固定网络预算；listen.address由online_config临时绑定。
    std::string listen_address = "127.0.0.1"; ///< 拥有数值IP，不解析DNS；地址族由内容推导。
    std::uint64_t run_ms = 0;        ///< [0,86400000]；0等待停止信号，非请求期限。
    std::int64_t shutdown_ms = 1000; ///< [0,86400000]网络排空期限，非worker join硬限。
};
/**
 * @brief 从默认值、可选JSON文件、CLI覆盖得到完整配置；失败不加载服务或启动线程
 * @param[in] arguments
 *     同步稳定借用argv[1..]的拥有副本；选项成对，--help由进程入口单独处理。
 *     --config最多一次；文件<=64KiB、普通文件、路径不含..且逐组件拒绝符号链接。
 *     文件schema_version=1，未知/重复键、嵌套、错误类型/范围拒绝；相对路径沿启动cwd。
 *
 * @return
 *     完整拥有值或Error；context为config.argument/file/json/value/required。
 *     文件每字段在CLI覆盖前检查，合并后再检查必需字段、资源相容及跨字段关系。
 * @throws
 *     分配及非预期库异常上传；失败局部fd/值由RAII释放，不返回部分配置。
 */
[[nodiscard]] base::Result<ServerSettings> load_server_settings(
    const std::vector<std::string> &arguments);
/**
 * @brief 为既有OnlineServer配置绑定数值地址借用，不执行I/O或改变源
 * @param[in] settings
 *     已加载且未修改的配置；返回值的listen.address借用settings.listen_address。
 *     调用者保持源稳定到OnlineServer创建完成，保守用法是持有到server销毁。
 *
 * @return
 *     独立配置值，地址族自动推导；不保存指向返回对象自身的借用。
 * @throws
 *     配置内拥有字符串的复制分配异常上传。
 */
[[nodiscard]] net::OnlineServerConfig online_config(const ServerSettings &settings);
} // namespace siftwing::config
