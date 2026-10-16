# 从公开输入复跑演示

在仓库根目录按 [README](../README.md) 完成 Release 构建，需要 Python 3 用于查看 JSON。全部输入来自[原创 MIT 夹具](../tests/fixtures/golden/README.md)，不需要额外词典、服务或私有数据。演示使用默认英文管线和关闭的缓存/期限。

## 建库与本地查询

终端一执行；输出目录每次新建，演示结束后保留结果，便于核对：

```sh
siftwing_demo=$(mktemp -d /tmp/siftwing-demo.XXXXXXXX)
build/release/siftwing-indexer \
  --output "$siftwing_demo" --version 1 --build-version public-demo \
  --search-txt tests/fixtures/golden/txt/en-river.txt \
  --search-rss tests/fixtures/golden/rss/original.xml \
  --recommendation-txt tests/fixtures/golden/txt/en-workbench.txt \
  --recommendation-rss tests/fixtures/golden/rss/original.xml \
  > "$siftwing_demo/indexer.txt" 2> "$siftwing_demo/indexer.stderr"
cat "$siftwing_demo/indexer.txt"
ls "$siftwing_demo/1"
build/release/siftwing-cli SEARCH --snapshot "$siftwing_demo" --version 1 \
  --query lantern --top-k 5 > "$siftwing_demo/local-search.json"
build/release/siftwing-cli SUGGEST --snapshot "$siftwing_demo" --version 1 \
  --key lantern --top-k 5 > "$siftwing_demo/local-suggest.json"
python3 -m json.tool "$siftwing_demo/local-search.json"
python3 -m json.tool "$siftwing_demo/local-suggest.json"
```

逐条检查退出码；正常建库/查询都为 0。indexer 输出 `key=value` 文本报告，可看到 `snapshot=1`、`durability=synced`，不作为 JSON 读取；stderr 可能包含夹具预期的回退/空正文等诊断。版本叶子目录 `1/` 恰有[格式说明](SNAPSHOT_FORMAT.md)中的 8 个普通文件，演示日志位于父目录，不加到版本目录中。SEARCH 返回非空 `results`；SUGGEST 的候选包含 `lantern` 且其编辑距离为 0。两种查询输出都报告 `schema_version=1`、`snapshot_version="1"`、`build_version="public-demo"`；版本身份在 JSON 中是字符串。

`public-demo` 只是演示标签；定位源码/程序须另记 `git rev-parse HEAD`、未提交 diff/文件哈希及 `sha256sum build/release/siftwing-*`。完整结果与计数以本次输出为准，不在此硬编码分数或建库数量。

## 启动服务与网络四命令

仍在终端一，使用内核分配端口避免与其他服务冲突：

```sh
build/release/siftwing-server --snapshot "$siftwing_demo" --version 1 \
  --port 0 --subs 2 --workers 4 --task-capacity 128 --shutdown-ms 1000 \
  > "$siftwing_demo/server.stdout" 2> "$siftwing_demo/server.stderr" &
siftwing_server_pid=$!
printf '演示目录: %s\n服务 PID: %s\n' "$siftwing_demo" "$siftwing_server_pid"
cat "$siftwing_demo/server.stdout"
```

等待出现 `PORT N`，`N` 是本次端口；未就绪时再次查看 stdout/stderr 并检查进程，不用端口 0 请求客户端。启动失败没有 PORT；不要把后台 shell 返回当作服务就绪。终端二从同一仓库根执行，把 `N` 替换为实际值：

```sh
siftwing_port=N
build/release/siftwing-cli --network PING --port "$siftwing_port"
build/release/siftwing-cli --network SERVER_INFO --port "$siftwing_port"
build/release/siftwing-cli --network SEARCH --port "$siftwing_port" \
  --query lantern --top-k 5
build/release/siftwing-cli --network SUGGEST --port "$siftwing_port" \
  --key lantern --top-k 5
```

PING 输出 `{}`；SERVER_INFO 的 `command` 为 `SERVER_INFO`，报告版本 `"1"`、构建标签 `public-demo` 和默认关闭缓存。SEARCH/SUGGEST 的业务 Body 与同参数本地查询一致；网络帧没有 CLI 为显示而追加的 LF，协议准入和退出码见[网络客户端](NETWORK_CLIENT.md)。客户端每条命令独立建连。

## 正常关闭与结果核对

回到终端一，仅操作上面保存的本次 PID：

```sh
kill -TERM "$siftwing_server_pid"
wait "$siftwing_server_pid"
siftwing_server_status=$?
printf '服务退出码: %s\n' "$siftwing_server_status"
cat "$siftwing_demo/server.stdout"
cat "$siftwing_demo/server.stderr"
```

预期服务退出 0，stdout 含 PORT 行与最终计数 JSON，stderr 含默认 info 结构化日志和关闭指标。SIGINT/SIGTERM 通过 signalfd 请求原优雅退出；输出计数不证明对端接收，1000ms 预算只限网络排空，不承诺进程总退出硬上限。首次核验后保留整个目录与退出码；不向输入夹具写回产物。

## 常见失败与能力边界

| 现象 | 核对与处理 |
|---|---|
| indexer 参数错误或已有版本 | 查看退出码和 stderr；输出父目录须已有，已有正式版本不能覆盖，重演用全新目录 |
| indexer 退出 3 | 版本已经发布但父目录持久性未确认；保留成品与诊断，不盲目重试同版本 |
| server 没有 PORT | 核对配置、精确版本、快照八文件及 stderr；启动失败不表示服务已接入 |
| 客户端参数错误/无法连接 | `--port` 须为实际 1..65535 端口；参数错误为 2，传输/协议失败为 1 |
| 客户端有效 ERROR_RESPONSE | 退出 5，按远端 status 分类；不能当成功查询或静默重试 |
| JSON/计数与预期不同 | 保留原 stdout/stderr、源码/程序身份，按[测试指南](TESTING.md)定位，勿手改成品或清单 |

该流程演示离线成品到真实 TCP 的可运行闭环，不做容量评估，也不触发在线热更新。后台加载和显式发布的 C++ API、旧请求保持与回滚由[安全发布](SNAPSHOT_PUBLICATION.md)及真实 `snapshot.publication` 测试验证；命令行没有 reload 入口，发送 SIGHUP 不能代替该控制流程。
