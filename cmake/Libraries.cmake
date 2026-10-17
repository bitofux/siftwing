# V1源码只在此编译；full仅归档V2/V3新增实现，通过PUBLIC复用V1。
add_library(siftwing_v1 STATIC
    src/document/document_reader.cc
    src/document/document_store.cc
    src/document/duplicate_decision.cc
    src/document/html_text.cc
    src/document/record_format.cc
    src/document/rss_reader.cc
    src/document/simhash.cc
    src/document/txt_reader.cc
    src/index/inverted_index.cc
    src/index/lexicon.cc
    src/index/posting.cc
    src/index/weighted_index.cc
    src/query/candidates.cc
    src/query/highlight.cc
    src/query/query.cc
    src/query/ranking.cc
    src/query/recommendation.cc
    src/query/summary.cc
    src/query/top_k.cc
    src/snapshot/manifest.cc
    src/snapshot/reader.cc
    src/snapshot/writer.cc
    src/text/cppjieba_tokenizer.cc
    src/text/english_tokenizer.cc
    src/text/normalize.cc
    src/text/term_frequency.cc
    src/text/text_pipeline.cc
)
target_link_libraries(siftwing_v1
    PUBLIC siftwing_base
    PRIVATE siftwing_utf8cpp siftwing_cppjieba siftwing_lexbor siftwing_tinyxml2
)
# 保留原排序/权重的严格浮点策略，避免扩大到无关翻译单元。
if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
    set_source_files_properties(src/index/weighted_index.cc src/query/ranking.cc
        PROPERTIES COMPILE_OPTIONS "-fno-fast-math;-ffp-contract=off")
endif()

add_library(siftwing_full STATIC
    src/cache/versioned_lru.cc
    src/concurrency/completion_queue.cc
    src/concurrency/routed_payload.cc
    src/concurrency/search_task.cc
    src/concurrency/worker_pool.cc
    src/config/server_settings.cc
    src/logging/logger.cc
    src/metrics/server_metrics.cc
    src/net/buffer.cc
    src/net/client.cc
    src/net/completion_wakeup.cc
    src/net/connection.cc
    src/net/connection_identity.cc
    src/net/deadline_queue.cc
    src/net/deadline_timer.cc
    src/net/epoll_ops.cc
    src/net/eventfd_wakeup.cc
    src/net/fd_handoff_queue.cc
    src/net/frame_codec.cc
    src/net/frame_decoder.cc
    src/net/listener.cc
    src/net/main_reactor.cc
    src/net/online_server.cc
    src/net/protocol_loop.cc
    src/net/protocol_sub_reactor.cc
    src/net/signalfd_source.cc
    src/net/socket_ops.cc
    src/net/sub_reactor.cc
    src/net/unique_fd.cc
    src/query/search_service.cc
    src/snapshot/loader.cc
    src/snapshot/manager.cc
)
target_link_libraries(siftwing_full
    PUBLIC siftwing_v1 Threads::Threads
    PRIVATE siftwing_utf8cpp siftwing_json siftwing_spdlog
)

# 构建探针独立于搜索版本库，保留最小工具链验证入口。
add_library(siftwing_bootstrap STATIC src/bootstrap/byte_probe.cc)
target_link_libraries(siftwing_bootstrap PUBLIC siftwing_base)

# 保留组件文档/旧调用方的链接名称；ALIAS不编译源码、不生成额外.a。
# 这些名称表达逻辑组件归属，实际链接边界是两个版本库。
add_library(siftwing_document INTERFACE)
target_link_libraries(siftwing_document INTERFACE siftwing_base)
add_library(siftwing_task_queue INTERFACE)
target_link_libraries(siftwing_task_queue INTERFACE siftwing_base Threads::Threads)

foreach(component IN ITEMS
    siftwing_document_reader
    siftwing_record_format
    siftwing_document_store
    siftwing_text_normalization
    siftwing_tokenizer
    siftwing_cppjieba_tokenizer
    siftwing_term_frequency
    siftwing_lexicon
    siftwing_posting
    siftwing_inverted_index
    siftwing_weights
    siftwing_snapshot_manifest
    siftwing_snapshot_writer
    siftwing_snapshot_reader
    siftwing_text_pipeline
    siftwing_query
    siftwing_candidates
    siftwing_ranking
    siftwing_top_k
    siftwing_summary
    siftwing_highlight
    siftwing_recommendation
    siftwing_simhash
    siftwing_duplicate_decision
    siftwing_txt_reader
    siftwing_html_text
    siftwing_rss_reader
)
    add_library(${component} ALIAS siftwing_v1)
endforeach()

foreach(component IN ITEMS
    siftwing_socket
    siftwing_listener
    siftwing_epoll
    siftwing_worker_pool
    siftwing_sub_reactor
    siftwing_fd_handoff_queue
    siftwing_eventfd_wakeup
    siftwing_deadline
    siftwing_signalfd
    siftwing_main_reactor
    siftwing_buffer
    siftwing_frame_decoder
    siftwing_metrics
    siftwing_logging
    siftwing_frame_codec
    siftwing_search_task
    siftwing_connection
    siftwing_completion_queue
    siftwing_connection_identity
    siftwing_routed_payload
    siftwing_protocol_sub_reactor
    siftwing_completion_wakeup
    siftwing_protocol_loop
    siftwing_snapshot_manager
    siftwing_snapshot_loader
    siftwing_client
    siftwing_versioned_lru
    siftwing_search_service
    siftwing_online_server
    siftwing_server_settings
)
    add_library(${component} ALIAS siftwing_full)
endforeach()
