/*
 * PROJECT : SIFTWING
 * FILE    : network.hpp
 * AUTHOR  : bitofux
 * DATE    : 2026-10-15
 * BRIEF   : CLI显式网络模式入口，不读取本地快照
 */
#pragma once
namespace siftwing::cli {
/** @brief 静态中文帮助；返回借用，进程生命内有效，无分配。 */
const char *network_help() noexcept;
/** @brief 解析--network命令并单次交换；返回CLI退出码，分配异常交原main处理。 */
int network_main(int argc, char **argv);
}
