# 保留真实测试注册、独立参考、故障注入链接选项和可选工具。
# 仅负载端使用原生线程；正式服务仍由siftwing-server提供，工具不链接替代业务实现。
if(SIFTWING_BUILD_BENCHMARKS)
    add_executable(siftwing_benchmark_loadgen tools/benchmark_loadgen.cc)
    target_link_libraries(siftwing_benchmark_loadgen PRIVATE siftwing_base siftwing_json Threads::Threads)
endif()
if(SIFTWING_BUILD_CODEC_FUZZER AND
   (NOT BUILD_TESTING OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR
    NOT SIFTWING_ENABLE_SANITIZERS))
    message(FATAL_ERROR "Codec libFuzzer requires BUILD_TESTING, Clang and SIFTWING_ENABLE_SANITIZERS")
endif()
if(BUILD_TESTING)
    foreach(kind IN ITEMS test concurrency_test failure_test)
        add_executable(siftwing_versioned_lru_${kind} tests/unit/versioned_lru_${kind}.cc)
        target_link_libraries(siftwing_versioned_lru_${kind} PRIVATE siftwing_versioned_lru)
        add_test(NAME cache.versioned_lru_${kind} COMMAND siftwing_versioned_lru_${kind})
        set_tests_properties(cache.versioned_lru_${kind} PROPERTIES TIMEOUT 30)
    endforeach()
    add_executable(siftwing_sub_deadlines_test tests/integration/sub_deadlines_test.cc)
    target_link_libraries(siftwing_sub_deadlines_test PRIVATE siftwing_protocol_sub_reactor)
    target_link_options(siftwing_sub_deadlines_test PRIVATE
        -Wl,--wrap=clock_gettime -Wl,--wrap=timerfd_settime -Wl,--wrap=recv -Wl,--wrap=send -Wl,--wrap=epoll_wait)
    add_test(NAME net.sub_deadlines COMMAND siftwing_sub_deadlines_test)
    set_tests_properties(net.sub_deadlines PROPERTIES TIMEOUT 45)
    add_executable(siftwing_deadline_queue_test tests/unit/deadline_queue_test.cc)
    target_link_libraries(siftwing_deadline_queue_test PRIVATE siftwing_deadline)
    add_test(NAME net.deadline_queue COMMAND siftwing_deadline_queue_test)
    add_executable(siftwing_deadline_timer_test tests/unit/deadline_timer_test.cc)
    target_link_libraries(siftwing_deadline_timer_test PRIVATE siftwing_deadline siftwing_epoll)
    target_link_options(siftwing_deadline_timer_test PRIVATE
        -Wl,--wrap=timerfd_create -Wl,--wrap=timerfd_settime -Wl,--wrap=clock_gettime -Wl,--wrap=read)
    add_test(NAME net.deadline_timer COMMAND siftwing_deadline_timer_test)
    add_executable(siftwing_sub_timer_test tests/integration/sub_timer_test.cc)
    target_link_libraries(siftwing_sub_timer_test PRIVATE siftwing_protocol_sub_reactor)
    target_link_options(siftwing_sub_timer_test PRIVATE
        -Wl,--wrap=timerfd_create -Wl,--wrap=epoll_ctl -Wl,--wrap=read -Wl,--wrap=close)
    add_test(NAME net.sub_timer COMMAND siftwing_sub_timer_test)
    set_tests_properties(net.deadline_queue net.deadline_timer net.sub_timer PROPERTIES TIMEOUT 30)
    add_executable(siftwing_completion_wakeup_test tests/integration/completion_wakeup_test.cc)
    target_link_libraries(siftwing_completion_wakeup_test PRIVATE siftwing_protocol_sub_reactor)
    add_test(NAME net.completion_wakeup COMMAND siftwing_completion_wakeup_test)
    add_executable(siftwing_completion_wakeup_concurrency_test tests/integration/completion_wakeup_concurrency_test.cc)
    target_link_libraries(siftwing_completion_wakeup_concurrency_test PRIVATE siftwing_protocol_sub_reactor siftwing_worker_pool)
    target_link_options(siftwing_completion_wakeup_concurrency_test PRIVATE -Wl,--wrap=send -Wl,--wrap=recv -Wl,--wrap=epoll_ctl)
    add_test(NAME net.completion_wakeup_concurrency COMMAND siftwing_completion_wakeup_concurrency_test)
    add_executable(siftwing_completion_wakeup_failure_test tests/integration/completion_wakeup_failure_test.cc)
    target_link_libraries(siftwing_completion_wakeup_failure_test PRIVATE siftwing_protocol_sub_reactor)
    target_link_options(siftwing_completion_wakeup_failure_test PRIVATE -Wl,--wrap=write -Wl,--wrap=pthread_mutex_lock -Wl,--wrap=epoll_wait)
    add_test(NAME net.completion_wakeup_failures COMMAND siftwing_completion_wakeup_failure_test)
    set_tests_properties(net.completion_wakeup net.completion_wakeup_concurrency net.completion_wakeup_failures PROPERTIES TIMEOUT 45)
    add_executable(siftwing_completion_queue_test tests/unit/completion_queue_test.cc)
    target_link_libraries(siftwing_completion_queue_test PRIVATE siftwing_completion_queue)
    add_test(NAME concurrency.completion_queue COMMAND siftwing_completion_queue_test)
    add_executable(siftwing_completion_queue_concurrency_test tests/unit/completion_queue_concurrency_test.cc)
    target_link_libraries(siftwing_completion_queue_concurrency_test PRIVATE siftwing_completion_queue siftwing_worker_pool)
    add_test(NAME concurrency.completion_queue_concurrency COMMAND siftwing_completion_queue_concurrency_test)
    add_executable(siftwing_completion_queue_failure_test tests/unit/completion_queue_failure_test.cc)
    target_link_libraries(siftwing_completion_queue_failure_test PRIVATE siftwing_completion_queue)
    target_link_options(siftwing_completion_queue_failure_test PRIVATE -Wl,--wrap=_Znwm -Wl,--wrap=pthread_mutex_lock)
    add_test(NAME concurrency.completion_queue_failures COMMAND siftwing_completion_queue_failure_test)
    set_tests_properties(concurrency.completion_queue concurrency.completion_queue_concurrency concurrency.completion_queue_failures
        PROPERTIES TIMEOUT 30)
    add_executable(siftwing_connection_identity_test tests/unit/connection_identity_test.cc)
    target_link_libraries(siftwing_connection_identity_test PRIVATE siftwing_routed_payload)
    target_link_options(siftwing_connection_identity_test PRIVATE -Wl,--wrap=_Znwm)
    add_test(NAME net.connection_identity COMMAND siftwing_connection_identity_test)
    add_executable(siftwing_connection_identity_reuse_test tests/integration/connection_identity_reuse_test.cc)
    target_link_libraries(siftwing_connection_identity_reuse_test PRIVATE siftwing_routed_payload siftwing_connection)
    add_test(NAME net.connection_identity_reuse COMMAND siftwing_connection_identity_reuse_test)
    add_executable(siftwing_connection_identity_handoff_test tests/unit/connection_identity_handoff_test.cc)
    target_link_libraries(siftwing_connection_identity_handoff_test PRIVATE siftwing_routed_payload siftwing_worker_pool)
    add_test(NAME net.connection_identity_handoff COMMAND siftwing_connection_identity_handoff_test)
    set_tests_properties(net.connection_identity net.connection_identity_reuse net.connection_identity_handoff
        PROPERTIES TIMEOUT 30)
    add_executable(siftwing_search_task_test tests/unit/search_task_test.cc)
    target_link_libraries(siftwing_search_task_test PRIVATE siftwing_search_task)
    add_test(NAME concurrency.search_task COMMAND siftwing_search_task_test)
    add_executable(siftwing_search_task_concurrency_test tests/unit/search_task_concurrency_test.cc)
    target_link_libraries(siftwing_search_task_concurrency_test PRIVATE siftwing_search_task siftwing_worker_pool)
    add_test(NAME concurrency.search_task_concurrency COMMAND siftwing_search_task_concurrency_test)
    add_executable(siftwing_search_task_failure_test tests/unit/search_task_failure_test.cc)
    target_link_libraries(siftwing_search_task_failure_test PRIVATE siftwing_search_task)
    add_test(NAME concurrency.search_task_failures COMMAND siftwing_search_task_failure_test)
    set_tests_properties(concurrency.search_task concurrency.search_task_concurrency
        concurrency.search_task_failures PROPERTIES TIMEOUT 30)
    add_executable(siftwing_worker_pool_test tests/unit/worker_pool_test.cc)
    target_link_libraries(siftwing_worker_pool_test PRIVATE siftwing_worker_pool)
    add_test(NAME concurrency.worker_pool COMMAND siftwing_worker_pool_test)
    add_executable(siftwing_worker_pool_concurrency_test tests/unit/worker_pool_concurrency_test.cc)
    target_link_libraries(siftwing_worker_pool_concurrency_test PRIVATE siftwing_worker_pool)
    add_test(NAME concurrency.worker_pool_concurrency COMMAND siftwing_worker_pool_concurrency_test)
    add_executable(siftwing_worker_pool_failure_test tests/unit/worker_pool_failure_test.cc)
    target_link_libraries(siftwing_worker_pool_failure_test PRIVATE siftwing_worker_pool)
    target_link_options(siftwing_worker_pool_failure_test PRIVATE
        -Wl,--wrap=_Znwm -Wl,--wrap=pthread_mutex_lock)
    add_test(NAME concurrency.worker_pool_failures COMMAND siftwing_worker_pool_failure_test)
    set_tests_properties(concurrency.worker_pool concurrency.worker_pool_concurrency
        concurrency.worker_pool_failures PROPERTIES TIMEOUT 30)
    add_executable(siftwing_task_queue_test tests/unit/task_queue_test.cc)
    target_link_libraries(siftwing_task_queue_test PRIVATE siftwing_task_queue)
    add_test(NAME concurrency.task_queue COMMAND siftwing_task_queue_test)
    add_executable(siftwing_task_queue_concurrency_test tests/unit/task_queue_concurrency_test.cc)
    target_link_libraries(siftwing_task_queue_concurrency_test PRIVATE siftwing_task_queue)
    add_test(NAME concurrency.task_queue_concurrency COMMAND siftwing_task_queue_concurrency_test)
    add_executable(siftwing_task_queue_failure_test tests/unit/task_queue_failure_test.cc)
    target_link_libraries(siftwing_task_queue_failure_test PRIVATE siftwing_task_queue)
    target_link_options(siftwing_task_queue_failure_test PRIVATE
        -Wl,--wrap=_Znwm -Wl,--wrap=pthread_mutex_lock)
    add_test(NAME concurrency.task_queue_failures COMMAND siftwing_task_queue_failure_test)
    set_tests_properties(concurrency.task_queue concurrency.task_queue_concurrency
        concurrency.task_queue_failures PROPERTIES TIMEOUT 30)
    add_executable(siftwing_multi_reactor_test tests/integration/multi_reactor_test.cc)
    target_link_libraries(siftwing_multi_reactor_test PRIVATE siftwing_protocol_sub_reactor)
    target_link_options(siftwing_multi_reactor_test PRIVATE
        -Wl,--wrap=accept4 -Wl,--wrap=epoll_ctl -Wl,--wrap=recv -Wl,--wrap=send -Wl,--wrap=close)
    add_test(NAME net.multi_reactor COMMAND siftwing_multi_reactor_test)
    add_executable(siftwing_multi_reactor_concurrency_test tests/integration/multi_reactor_concurrency_test.cc)
    target_link_libraries(siftwing_multi_reactor_concurrency_test PRIVATE siftwing_protocol_sub_reactor)
    add_test(NAME net.multi_reactor_concurrency COMMAND siftwing_multi_reactor_concurrency_test)
    add_executable(siftwing_multi_reactor_failure_test tests/integration/multi_reactor_failure_test.cc)
    target_link_libraries(siftwing_multi_reactor_failure_test PRIVATE siftwing_protocol_sub_reactor)
    target_link_options(siftwing_multi_reactor_failure_test PRIVATE
        -Wl,--wrap=epoll_create1 -Wl,--wrap=eventfd -Wl,--wrap=socket -Wl,--wrap=accept4
        -Wl,--wrap=epoll_ctl -Wl,--wrap=epoll_wait -Wl,--wrap=write -Wl,--wrap=recv -Wl,--wrap=close -Wl,--wrap=_Znwm)
    add_test(NAME net.multi_reactor_failures COMMAND siftwing_multi_reactor_failure_test)
    set_tests_properties(net.multi_reactor net.multi_reactor_concurrency net.multi_reactor_failures PROPERTIES TIMEOUT 45)
    add_executable(siftwing_signalfd_test tests/unit/signalfd_test.cc)
    target_link_libraries(siftwing_signalfd_test PRIVATE siftwing_main_reactor)
    target_link_options(siftwing_signalfd_test PRIVATE -Wl,--wrap=pthread_sigmask -Wl,--wrap=signalfd
        -Wl,--wrap=read -Wl,--wrap=epoll_ctl -Wl,--wrap=epoll_wait -Wl,--wrap=close)
    add_test(NAME net.signalfd COMMAND siftwing_signalfd_test)
    set_tests_properties(net.signalfd PROPERTIES TIMEOUT 30)

    add_executable(siftwing_main_reactor_test tests/unit/main_reactor_test.cc)
    target_link_libraries(siftwing_main_reactor_test PRIVATE siftwing_main_reactor)
    add_test(NAME net.main_reactor COMMAND siftwing_main_reactor_test)
    add_executable(siftwing_main_reactor_concurrency_test tests/unit/main_reactor_concurrency_test.cc)
    target_link_libraries(siftwing_main_reactor_concurrency_test PRIVATE siftwing_main_reactor)
    target_link_options(siftwing_main_reactor_concurrency_test PRIVATE -Wl,--wrap=write -Wl,--wrap=epoll_wait)
    add_test(NAME net.main_reactor_concurrency COMMAND siftwing_main_reactor_concurrency_test)
    add_executable(siftwing_main_reactor_failure_test tests/unit/main_reactor_failure_test.cc)
    target_link_libraries(siftwing_main_reactor_failure_test PRIVATE siftwing_main_reactor)
    target_link_options(siftwing_main_reactor_failure_test PRIVATE
        -Wl,--wrap=socket -Wl,--wrap=epoll_create1 -Wl,--wrap=epoll_ctl -Wl,--wrap=eventfd
        -Wl,--wrap=epoll_wait -Wl,--wrap=accept4 -Wl,--wrap=setsockopt -Wl,--wrap=write
        -Wl,--wrap=close -Wl,--wrap=pthread_mutex_lock -Wl,--wrap=_Znwm)
    add_test(NAME net.main_reactor_failures COMMAND siftwing_main_reactor_failure_test)
    set_tests_properties(net.main_reactor net.main_reactor_concurrency
        net.main_reactor_failures PROPERTIES TIMEOUT 30)
    add_executable(siftwing_eventfd_wakeup_test tests/unit/eventfd_wakeup_test.cc)
    target_link_libraries(siftwing_eventfd_wakeup_test PRIVATE siftwing_eventfd_wakeup siftwing_epoll)
    add_test(NAME net.eventfd_wakeup COMMAND siftwing_eventfd_wakeup_test)
    add_executable(siftwing_eventfd_wakeup_concurrency_test tests/unit/eventfd_wakeup_concurrency_test.cc)
    target_link_libraries(siftwing_eventfd_wakeup_concurrency_test PRIVATE siftwing_eventfd_wakeup siftwing_epoll)
    add_test(NAME net.eventfd_wakeup_concurrency COMMAND siftwing_eventfd_wakeup_concurrency_test)
    add_executable(siftwing_eventfd_wakeup_failure_test tests/unit/eventfd_wakeup_failure_test.cc)
    target_link_libraries(siftwing_eventfd_wakeup_failure_test PRIVATE siftwing_eventfd_wakeup)
    target_link_options(siftwing_eventfd_wakeup_failure_test PRIVATE
        -Wl,--wrap=eventfd -Wl,--wrap=read -Wl,--wrap=write -Wl,--wrap=close -Wl,--wrap=pthread_mutex_lock)
    add_test(NAME net.eventfd_wakeup_failures COMMAND siftwing_eventfd_wakeup_failure_test)
    set_tests_properties(net.eventfd_wakeup net.eventfd_wakeup_concurrency
        net.eventfd_wakeup_failures PROPERTIES TIMEOUT 30)
    add_executable(siftwing_fd_handoff_queue_test tests/unit/fd_handoff_queue_test.cc)
    target_link_libraries(siftwing_fd_handoff_queue_test PRIVATE siftwing_fd_handoff_queue)
    add_test(NAME net.fd_handoff_queue COMMAND siftwing_fd_handoff_queue_test)
    add_executable(siftwing_fd_handoff_queue_concurrency_test tests/unit/fd_handoff_queue_concurrency_test.cc)
    target_link_libraries(siftwing_fd_handoff_queue_concurrency_test PRIVATE siftwing_fd_handoff_queue)
    add_test(NAME net.fd_handoff_queue_concurrency COMMAND siftwing_fd_handoff_queue_concurrency_test)
    add_executable(siftwing_fd_handoff_queue_failure_test tests/unit/fd_handoff_queue_failure_test.cc)
    target_link_libraries(siftwing_fd_handoff_queue_failure_test PRIVATE siftwing_fd_handoff_queue)
    target_link_options(siftwing_fd_handoff_queue_failure_test PRIVATE
        -Wl,--wrap=close -Wl,--wrap=_Znwm -Wl,--wrap=pthread_mutex_lock)
    add_test(NAME net.fd_handoff_queue_failures COMMAND siftwing_fd_handoff_queue_failure_test)
    set_tests_properties(net.fd_handoff_queue net.fd_handoff_queue_concurrency
        net.fd_handoff_queue_failures PROPERTIES TIMEOUT 30)
    add_executable(siftwing_sub_reactor_test tests/unit/sub_reactor_test.cc)
    target_link_libraries(siftwing_sub_reactor_test PRIVATE siftwing_sub_reactor)
    add_test(NAME net.sub_reactor COMMAND siftwing_sub_reactor_test)
    add_executable(siftwing_sub_reactor_failure_test tests/unit/sub_reactor_failure_test.cc)
    target_link_libraries(siftwing_sub_reactor_failure_test PRIVATE siftwing_sub_reactor)
    target_link_options(siftwing_sub_reactor_failure_test PRIVATE
        -Wl,--wrap=epoll_create1 -Wl,--wrap=epoll_wait -Wl,--wrap=close -Wl,--wrap=_Znwm)
    add_test(NAME net.sub_reactor_failures COMMAND siftwing_sub_reactor_failure_test)
    set_tests_properties(net.sub_reactor net.sub_reactor_failures PROPERTIES TIMEOUT 30)
    add_executable(siftwing_protocol_loop_test tests/unit/protocol_loop_test.cc)
    target_link_libraries(siftwing_protocol_loop_test PRIVATE siftwing_protocol_loop)
    add_test(NAME net.protocol_loop COMMAND siftwing_protocol_loop_test)
    add_executable(siftwing_protocol_loop_failure_test tests/unit/protocol_loop_failure_test.cc)
    target_link_libraries(siftwing_protocol_loop_failure_test PRIVATE siftwing_protocol_loop)
    target_link_options(siftwing_protocol_loop_failure_test PRIVATE
        -Wl,--wrap=socket -Wl,--wrap=epoll_create1 -Wl,--wrap=epoll_ctl -Wl,--wrap=epoll_wait
        -Wl,--wrap=accept4 -Wl,--wrap=recv -Wl,--wrap=send -Wl,--wrap=close)
    add_test(NAME net.protocol_loop_failures COMMAND siftwing_protocol_loop_failure_test)
    set_tests_properties(net.protocol_loop net.protocol_loop_failures PROPERTIES TIMEOUT 30)
    add_executable(siftwing_connection_write_test tests/unit/connection_write_test.cc)
    target_link_libraries(siftwing_connection_write_test PRIVATE siftwing_connection siftwing_listener)
    add_test(NAME net.connection_write COMMAND siftwing_connection_write_test)
    add_executable(siftwing_connection_write_failure_test tests/unit/connection_write_failure_test.cc)
    target_link_libraries(siftwing_connection_write_failure_test PRIVATE siftwing_connection)
    target_link_options(siftwing_connection_write_failure_test PRIVATE -Wl,--wrap=send -Wl,--wrap=close)
    add_test(NAME net.connection_write_failures COMMAND siftwing_connection_write_failure_test)
    set_tests_properties(net.connection_write net.connection_write_failures PROPERTIES TIMEOUT 30)
    add_executable(siftwing_connection_read_test tests/unit/connection_read_test.cc)
    target_link_libraries(siftwing_connection_read_test PRIVATE siftwing_connection siftwing_listener)
    add_test(NAME net.connection_read COMMAND siftwing_connection_read_test)
    set_tests_properties(net.connection_read PROPERTIES TIMEOUT 30)
    add_executable(siftwing_connection_failure_test tests/unit/connection_failure_test.cc)
    target_link_libraries(siftwing_connection_failure_test PRIVATE siftwing_connection)
    target_link_options(siftwing_connection_failure_test PRIVATE -Wl,--wrap=recv -Wl,--wrap=close)
    add_test(NAME net.connection_failures COMMAND siftwing_connection_failure_test)
    set_tests_properties(net.connection_failures PROPERTIES TIMEOUT 30)
    add_executable(siftwing_listener_test tests/unit/listener_test.cc)
    target_link_libraries(siftwing_listener_test PRIVATE siftwing_listener siftwing_epoll)
    add_test(NAME net.listener COMMAND siftwing_listener_test)
    set_tests_properties(net.listener PROPERTIES TIMEOUT 30)
    add_executable(siftwing_listener_failure_test tests/unit/listener_failure_test.cc)
    target_link_libraries(siftwing_listener_failure_test PRIVATE siftwing_listener)
    target_link_options(siftwing_listener_failure_test PRIVATE
        -Wl,--wrap=socket -Wl,--wrap=setsockopt -Wl,--wrap=bind -Wl,--wrap=listen
        -Wl,--wrap=getsockname -Wl,--wrap=accept4 -Wl,--wrap=close)
    add_test(NAME net.listener_failures COMMAND siftwing_listener_failure_test)
    set_tests_properties(net.listener_failures PROPERTIES TIMEOUT 30)
    add_executable(siftwing_epoll_ops_test tests/unit/epoll_ops_test.cc)
    target_link_libraries(siftwing_epoll_ops_test PRIVATE siftwing_epoll)
    add_test(NAME net.epoll_ops COMMAND siftwing_epoll_ops_test)
    set_tests_properties(net.epoll_ops PROPERTIES TIMEOUT 30)
    add_executable(siftwing_epoll_failure_test tests/unit/epoll_failure_test.cc)
    target_link_libraries(siftwing_epoll_failure_test PRIVATE siftwing_epoll)
    target_link_options(siftwing_epoll_failure_test PRIVATE
        -Wl,--wrap=epoll_create1 -Wl,--wrap=epoll_ctl)
    add_test(NAME net.epoll_failures COMMAND siftwing_epoll_failure_test)
    set_tests_properties(net.epoll_failures PROPERTIES TIMEOUT 30)
    add_executable(siftwing_socket_ops_test tests/unit/socket_ops_test.cc)
    target_link_libraries(siftwing_socket_ops_test PRIVATE siftwing_socket)
    add_test(NAME net.socket_ops COMMAND siftwing_socket_ops_test)
    set_tests_properties(net.socket_ops PROPERTIES TIMEOUT 30)
    add_executable(siftwing_socket_failure_test tests/unit/socket_failure_test.cc)
    target_link_libraries(siftwing_socket_failure_test PRIVATE siftwing_socket)
    target_link_options(siftwing_socket_failure_test PRIVATE
        -Wl,--wrap=socket -Wl,--wrap=close -Wl,--wrap=recv -Wl,--wrap=send)
    add_test(NAME net.socket_failures COMMAND siftwing_socket_failure_test)
    set_tests_properties(net.socket_failures PROPERTIES TIMEOUT 30)
    add_test(NAME net.socket_compile_contract COMMAND "${CMAKE_COMMAND}"
        "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/socket_compile_contract.cmake")
    set_tests_properties(net.socket_compile_contract PROPERTIES TIMEOUT 30)
    add_library(siftwing_codec_properties STATIC tests/fuzz/frame_codec_properties.cc)
    target_link_libraries(siftwing_codec_properties PUBLIC siftwing_frame_codec)
    add_executable(siftwing_codec_mutation_test tests/fuzz/frame_codec_mutation_test.cc)
    target_link_libraries(siftwing_codec_mutation_test PRIVATE siftwing_codec_properties)
    add_test(NAME net.codec_mutations COMMAND siftwing_codec_mutation_test)
    set_tests_properties(net.codec_mutations PROPERTIES TIMEOUT 120)
    add_executable(siftwing_codec_reference_probe tests/integration/frame_codec_probe.cc)
    target_link_libraries(siftwing_codec_reference_probe PRIVATE siftwing_frame_codec)
    if(SIFTWING_BUILD_CODEC_FUZZER)
        # 在专用目标直接重编译全部codec源码，使覆盖反馈进入实现，不只覆盖入口。
        add_executable(siftwing_frame_codec_fuzzer tests/fuzz/frame_codec_fuzzer.cc
            tests/fuzz/frame_codec_properties.cc src/net/buffer.cc
            src/net/frame_decoder.cc src/net/frame_codec.cc)
        target_link_libraries(siftwing_frame_codec_fuzzer PRIVATE siftwing_base
            siftwing_utf8cpp siftwing_json)
        target_compile_options(siftwing_frame_codec_fuzzer PRIVATE -fsanitize=fuzzer-no-link)
        target_link_options(siftwing_frame_codec_fuzzer PRIVATE -fsanitize=fuzzer)
    endif()
    add_executable(siftwing_frame_codec_test tests/unit/frame_codec_test.cc)
    target_link_libraries(siftwing_frame_codec_test PRIVATE siftwing_frame_codec)
    add_test(NAME net.frame_codec COMMAND siftwing_frame_codec_test)
    set_tests_properties(net.frame_codec PROPERTIES TIMEOUT 30)
    add_executable(siftwing_frame_decoder_test tests/unit/frame_decoder_test.cc)
    target_link_libraries(siftwing_frame_decoder_test PRIVATE siftwing_frame_decoder)
    add_test(NAME net.frame_decoder COMMAND siftwing_frame_decoder_test)
    set_tests_properties(net.frame_decoder PROPERTIES TIMEOUT 30)
    add_executable(siftwing_buffer_test tests/unit/buffer_test.cc)
    target_link_libraries(siftwing_buffer_test PRIVATE siftwing_buffer)
    add_test(NAME net.buffer COMMAND siftwing_buffer_test)
    set_tests_properties(net.buffer PROPERTIES TIMEOUT 30)
    find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
    if(SIFTWING_BUILD_BENCHMARKS)
        add_test(NAME benchmark.loadgen_contract COMMAND Python3::Interpreter -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/benchmark_loadgen_test.py"
            --native "$<TARGET_FILE:siftwing_benchmark_loadgen>")
        set_tests_properties(benchmark.loadgen_contract PROPERTIES TIMEOUT 45)
    endif()
    add_executable(siftwing_output_backpressure_test tests/integration/output_backpressure_test.cc)
    target_link_libraries(siftwing_output_backpressure_test PRIVATE siftwing_protocol_sub_reactor)
    target_link_options(siftwing_output_backpressure_test PRIVATE -Wl,--wrap=recv -Wl,--wrap=send -Wl,--wrap=epoll_ctl -Wl,--wrap=accept4)
    foreach(suite IN ITEMS limits sub tcp)
        add_test(NAME net.output_backpressure_${suite} COMMAND siftwing_output_backpressure_test ${suite})
        set_tests_properties(net.output_backpressure_${suite} PROPERTIES TIMEOUT 30)
    endforeach()
    add_executable(siftwing_single_inflight_test tests/integration/single_inflight_test.cc)
    target_link_libraries(siftwing_single_inflight_test PRIVATE siftwing_protocol_sub_reactor siftwing_worker_pool)
    target_link_options(siftwing_single_inflight_test PRIVATE -Wl,--wrap=recv -Wl,--wrap=send -Wl,--wrap=epoll_ctl)
    foreach(suite IN ITEMS worker output errors)
        add_test(NAME net.single_inflight_${suite} COMMAND siftwing_single_inflight_test ${suite})
        set_tests_properties(net.single_inflight_${suite} PROPERTIES TIMEOUT 30)
    endforeach()
    add_executable(siftwing_online_server_failure_test tests/integration/online_server_failure_test.cc)
    add_executable(siftwing_single_reactor_test tests/integration/single_reactor_test.cc)
    target_link_libraries(siftwing_single_reactor_test PRIVATE siftwing_online_server)
    target_link_options(siftwing_single_reactor_test PRIVATE -Wl,--wrap=epoll_ctl)
    add_test(NAME net.single_reactor_wiring COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/worker_saturation_test.py"
        --indexer "$<TARGET_FILE:siftwing_indexer>" --writer "$<TARGET_FILE:siftwing_writer_probe>"
        --native "$<TARGET_FILE:siftwing_single_reactor_test>" --subs 1)
    set_tests_properties(net.single_reactor_wiring PROPERTIES TIMEOUT 60)
    target_link_libraries(siftwing_online_server_failure_test PRIVATE siftwing_online_server)
    target_link_options(siftwing_online_server_failure_test PRIVATE -Wl,--wrap=write -Wl,--wrap=_Znwm -Wl,--wrap=send -Wl,--wrap=recv)
    add_executable(siftwing_versioned_cache_test tests/integration/versioned_cache_test.cc)
    target_link_libraries(siftwing_versioned_cache_test PRIVATE siftwing_online_server siftwing_json)
    foreach(subs IN ITEMS 1 2)
        add_test(NAME cache.service_${subs} COMMAND Python3::Interpreter -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/versioned_cache_test.py"
            --indexer "$<TARGET_FILE:siftwing_indexer>" --writer "$<TARGET_FILE:siftwing_writer_probe>"
            --native "$<TARGET_FILE:siftwing_versioned_cache_test>" --subs ${subs})
        set_tests_properties(cache.service_${subs} PROPERTIES TIMEOUT 60)
    endforeach()
    add_executable(siftwing_online_deadlines_test tests/integration/online_deadlines_test.cc)
    target_link_libraries(siftwing_online_deadlines_test PRIVATE siftwing_online_server siftwing_json)
    target_link_options(siftwing_online_deadlines_test PRIVATE
        -Wl,--wrap=_ZNK8siftwing5query13SearchService7executeB5cxx11ERKNS_11concurrency10SearchTaskEPNS0_16CacheObservationEPSt8optionalImE)
    foreach(subs IN ITEMS 1 2)
        add_test(NAME net.online_deadlines_${subs} COMMAND Python3::Interpreter -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/worker_saturation_test.py"
            --indexer "$<TARGET_FILE:siftwing_indexer>" --writer "$<TARGET_FILE:siftwing_writer_probe>"
            --native "$<TARGET_FILE:siftwing_online_deadlines_test>" --subs ${subs})
        set_tests_properties(net.online_deadlines_${subs} PROPERTIES TIMEOUT 60)
    endforeach()
    add_executable(siftwing_v2_fault_concurrency_test tests/integration/v2_fault_concurrency_test.cc)
    target_link_libraries(siftwing_v2_fault_concurrency_test PRIVATE siftwing_online_server siftwing_json)
    target_link_options(siftwing_v2_fault_concurrency_test PRIVATE
        -Wl,--wrap=_ZNK8siftwing5query13SearchService7executeB5cxx11ERKNS_11concurrency10SearchTaskEPNS0_16CacheObservationEPSt8optionalImE
        -Wl,--wrap=_ZN8siftwing11concurrency10WorkerPool10try_submitERSt10unique_ptrINS0_10WorkerTaskESt14default_deleteIS3_EE
        -Wl,--wrap=_ZN8siftwing11concurrency16RoutedSearchTask6createENS_3net12ConnectionIdERNS0_10SearchTaskE
        -Wl,--wrap=accept4 -Wl,--wrap=send -Wl,--wrap=recv -Wl,--wrap=epoll_ctl)
    foreach(subs IN ITEMS 1 2)
        add_test(NAME net.v2_fault_concurrency_${subs} COMMAND Python3::Interpreter -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/worker_saturation_test.py"
            --indexer "$<TARGET_FILE:siftwing_indexer>" --writer "$<TARGET_FILE:siftwing_writer_probe>"
            --native "$<TARGET_FILE:siftwing_v2_fault_concurrency_test>" --subs ${subs})
        set_tests_properties(net.v2_fault_concurrency_${subs} PROPERTIES TIMEOUT 60)
    endforeach()
    add_executable(siftwing_graceful_sub_test tests/integration/graceful_sub_test.cc)
    target_link_libraries(siftwing_graceful_sub_test PRIVATE siftwing_protocol_sub_reactor)
    target_link_options(siftwing_graceful_sub_test PRIVATE
        -Wl,--wrap=clock_gettime -Wl,--wrap=send -Wl,--wrap=write -Wl,--wrap=timerfd_settime -Wl,--wrap=epoll_wait)
    foreach(suite IN ITEMS drain boundaries faults timer)
        add_test(NAME net.graceful_sub_${suite} COMMAND siftwing_graceful_sub_test ${suite})
        set_tests_properties(net.graceful_sub_${suite} PROPERTIES TIMEOUT 30)
    endforeach()
    add_executable(siftwing_graceful_server_test tests/integration/graceful_server_test.cc)
    target_link_libraries(siftwing_graceful_server_test PRIVATE siftwing_online_server siftwing_json)
    target_link_options(siftwing_graceful_server_test PRIVATE
        -Wl,--wrap=_ZNK8siftwing5query13SearchService7executeB5cxx11ERKNS_11concurrency10SearchTaskEPNS0_16CacheObservationEPSt8optionalImE
        -Wl,--wrap=_ZN8siftwing11concurrency10WorkerPool12request_stopEv
        -Wl,--wrap=_ZN8siftwing11concurrency10WorkerPool4joinEv
        -Wl,--wrap=_ZN8siftwing3net18arm_deadline_timerEiSt8optionalIlE
        -Wl,--wrap=epoll_ctl -Wl,--wrap=close
        -Wl,--wrap=write -Wl,--wrap=clock_gettime)
    foreach(subs IN ITEMS 1 2)
        add_test(NAME net.graceful_server_${subs} COMMAND Python3::Interpreter -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/worker_saturation_test.py"
            --indexer "$<TARGET_FILE:siftwing_indexer>" --writer "$<TARGET_FILE:siftwing_writer_probe>"
            --native "$<TARGET_FILE:siftwing_graceful_server_test>" --subs ${subs})
        set_tests_properties(net.graceful_server_${subs} PROPERTIES TIMEOUT 60)
    endforeach()
    add_executable(siftwing_signalfd_server_probe apps/server/main.cc tests/support/signalfd_server_probe.cc)
    target_link_libraries(siftwing_signalfd_server_probe PRIVATE siftwing_server_settings)
    target_link_options(siftwing_signalfd_server_probe PRIVATE -Wl,--wrap=pthread_sigmask
        -Wl,--wrap=pthread_create -Wl,--wrap=signalfd -Wl,--wrap=epoll_ctl -Wl,--wrap=close)
    add_test(NAME net.signalfd_process COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/signalfd_process_test.py"
        --server "$<TARGET_FILE:siftwing_server>" --probe "$<TARGET_FILE:siftwing_signalfd_server_probe>"
        --indexer "$<TARGET_FILE:siftwing_indexer>" --writer "$<TARGET_FILE:siftwing_writer_probe>")
    set_tests_properties(net.signalfd_process PROPERTIES TIMEOUT 90)
    add_test(NAME net.graceful_process COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/graceful_process_test.py"
        --server "$<TARGET_FILE:siftwing_server>" --indexer "$<TARGET_FILE:siftwing_indexer>"
        --writer "$<TARGET_FILE:siftwing_writer_probe>")
    set_tests_properties(net.graceful_process PROPERTIES TIMEOUT 90)
    add_executable(siftwing_worker_saturation_test tests/integration/worker_saturation_test.cc)
    target_link_libraries(siftwing_worker_saturation_test PRIVATE siftwing_online_server siftwing_json)
    target_link_options(siftwing_worker_saturation_test PRIVATE
        -Wl,--wrap=_ZNK8siftwing5query13SearchService7executeB5cxx11ERKNS_11concurrency10SearchTaskEPNS0_16CacheObservationEPSt8optionalImE
        -Wl,--wrap=_ZN8siftwing11concurrency10WorkerPool10try_submitERSt10unique_ptrINS0_10WorkerTaskESt14default_deleteIS3_EE)
    foreach(subs IN ITEMS 1 2)
        add_test(NAME net.worker_saturation_${subs} COMMAND Python3::Interpreter -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/worker_saturation_test.py"
            --indexer "$<TARGET_FILE:siftwing_indexer>" --writer "$<TARGET_FILE:siftwing_writer_probe>"
            --native "$<TARGET_FILE:siftwing_worker_saturation_test>" --subs ${subs})
        set_tests_properties(net.worker_saturation_${subs} PROPERTIES TIMEOUT 60)
    endforeach()
    add_executable(siftwing_network_client_test tests/unit/network_client_test.cc)
    target_link_libraries(siftwing_network_client_test PRIVATE siftwing_client Threads::Threads)
    target_link_options(siftwing_network_client_test PRIVATE -Wl,--wrap=send -Wl,--wrap=recv -Wl,--wrap=poll)
    add_test(NAME net.client_native COMMAND siftwing_network_client_test)
    set_tests_properties(net.client_native PROPERTIES TIMEOUT 30)
    add_test(NAME logging.production COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/structured_logging_test.py"
        --server "$<TARGET_FILE:siftwing_server>" --indexer "$<TARGET_FILE:siftwing_indexer>"
        --writer "$<TARGET_FILE:siftwing_writer_probe>")
    set_tests_properties(logging.production PROPERTIES TIMEOUT 90)

    add_executable(siftwing_server_metrics_test tests/unit/server_metrics_test.cc)
    target_link_libraries(siftwing_server_metrics_test PRIVATE siftwing_metrics siftwing_logging siftwing_connection siftwing_json)
    target_link_options(siftwing_server_metrics_test PRIVATE -Wl,--wrap=_Znwm -Wl,--wrap=pthread_mutex_lock)
    add_test(NAME metrics.contract COMMAND siftwing_server_metrics_test)
    set_tests_properties(metrics.contract PROPERTIES TIMEOUT 60)
    add_test(NAME metrics.production COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/server_metrics_test.py"
        --server "$<TARGET_FILE:siftwing_server>" --indexer "$<TARGET_FILE:siftwing_indexer>"
        --writer "$<TARGET_FILE:siftwing_writer_probe>")
    set_tests_properties(metrics.production PROPERTIES TIMEOUT 60)

    add_executable(siftwing_logging_test tests/unit/logging_test.cc)
    target_link_libraries(siftwing_logging_test PRIVATE siftwing_logging siftwing_json Threads::Threads)
    target_link_options(siftwing_logging_test PRIVATE -Wl,--wrap=write)
    add_test(NAME logging.contract COMMAND siftwing_logging_test)
    set_tests_properties(logging.contract PROPERTIES TIMEOUT 30)

    add_executable(siftwing_server_settings_test tests/unit/server_settings_test.cc)
    target_link_libraries(siftwing_server_settings_test PRIVATE siftwing_server_settings)
    target_link_options(siftwing_server_settings_test PRIVATE -Wl,--wrap=read)
    add_test(NAME config.server_settings COMMAND siftwing_server_settings_test)
    set_tests_properties(config.server_settings PROPERTIES TIMEOUT 30)
    add_test(NAME config.server_process COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/server_config_test.py"
        --server "$<TARGET_FILE:siftwing_server>" --writer "$<TARGET_FILE:siftwing_writer_probe>"
        --indexer "$<TARGET_FILE:siftwing_indexer>")
    set_tests_properties(config.server_process PROPERTIES TIMEOUT 120)
    foreach(suite IN ITEMS integration protocol arguments)
        add_test(NAME net.client_${suite} COMMAND Python3::Interpreter -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/network_client_test.py"
            --cli "$<TARGET_FILE:siftwing_cli>" --server "$<TARGET_FILE:siftwing_server>"
            --indexer "$<TARGET_FILE:siftwing_indexer>" --writer "$<TARGET_FILE:siftwing_writer_probe>"
            --suite ${suite})
        set_tests_properties(net.client_${suite} PROPERTIES TIMEOUT 120)
    endforeach()
    foreach(suite IN ITEMS integration concurrency failures native)
        add_test(NAME net.online_search_${suite} COMMAND Python3::Interpreter -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/online_search_test.py"
            --server "$<TARGET_FILE:siftwing_server>" --indexer "$<TARGET_FILE:siftwing_indexer>"
            --writer "$<TARGET_FILE:siftwing_writer_probe>" --native "$<TARGET_FILE:siftwing_online_server_failure_test>"
            --suite ${suite})
        set_tests_properties(net.online_search_${suite} PROPERTIES TIMEOUT 120)
    endforeach()
    add_test(NAME net.protocol_probe_process COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/protocol_probe_test.py" $<TARGET_FILE:siftwing_protocol_probe>)
    set_tests_properties(net.protocol_probe_process PROPERTIES TIMEOUT 30)
    add_test(NAME net.codec_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/frame_codec_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_codec_reference_probe>")
    set_tests_properties(net.codec_reference PROPERTIES TIMEOUT 120)
    add_executable(siftwing_byte_probe_test tests/unit/byte_probe_test.cc)
    target_link_libraries(siftwing_byte_probe_test PRIVATE siftwing_bootstrap)
    add_test(NAME bootstrap.byte_probe COMMAND siftwing_byte_probe_test)
    add_test(NAME bootstrap.cli
        COMMAND "${CMAKE_COMMAND}" "-DPROBE=$<TARGET_FILE:siftwing_build_probe>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/build_probe_cli.cmake"
    )
    set_tests_properties(bootstrap.byte_probe bootstrap.cli PROPERTIES TIMEOUT 10)
    add_test(NAME app.indexer_integration COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/indexer_test.py"
        --indexer "$<TARGET_FILE:siftwing_indexer>" --reader "$<TARGET_FILE:siftwing_reader_probe>" --suite integration)
    add_test(NAME app.indexer_failures COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/indexer_test.py"
        --indexer "$<TARGET_FILE:siftwing_indexer>" --reader "$<TARGET_FILE:siftwing_reader_probe>" --suite failures)
    set_tests_properties(app.indexer_integration app.indexer_failures PROPERTIES TIMEOUT 120)
    add_test(NAME app.cli_integration COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/cli_test.py"
        --cli "$<TARGET_FILE:siftwing_cli>" --indexer "$<TARGET_FILE:siftwing_indexer>"
        --writer "$<TARGET_FILE:siftwing_writer_probe>" --suite integration)
    add_test(NAME app.cli_failures COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/cli_test.py"
        --cli "$<TARGET_FILE:siftwing_cli>" --indexer "$<TARGET_FILE:siftwing_indexer>"
        --writer "$<TARGET_FILE:siftwing_writer_probe>" --suite failures)
    set_tests_properties(app.cli_integration app.cli_failures PROPERTIES TIMEOUT 120)
    # 固定黄金走正式应用；探针仅观察同进程生命周期，不能以进程退出替代资源交接证明。
    add_executable(siftwing_v1_lifecycle_probe tests/integration/v1_lifecycle_probe.cc)
    target_link_libraries(siftwing_v1_lifecycle_probe PRIVATE siftwing_snapshot_reader siftwing_top_k
        siftwing_summary siftwing_highlight siftwing_recommendation siftwing_cppjieba_tokenizer)
    foreach(suite IN ITEMS golden rejection lifecycle)
        add_test(NAME v1.${suite} COMMAND Python3::Interpreter -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/v1_golden_test.py"
            --indexer "$<TARGET_FILE:siftwing_indexer>" --cli "$<TARGET_FILE:siftwing_v1_cli>"
            --probe "$<TARGET_FILE:siftwing_v1_lifecycle_probe>"
            --fixtures "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/golden"
            --data "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/v1" --suite "${suite}")
        set_tests_properties(v1.${suite} PROPERTIES TIMEOUT 120)
    endforeach()
    add_test(NAME fixtures.integrity
        COMMAND Python3::Interpreter -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/golden_fixtures_test.py"
            --fixtures "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/golden"
            --project-license "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    )
    set_tests_properties(fixtures.integrity PROPERTIES TIMEOUT 15)

    add_executable(siftwing_result_test tests/unit/result_test.cc)
    target_link_libraries(siftwing_result_test PRIVATE siftwing_base)
    add_test(NAME base.result COMMAND siftwing_result_test)
    set_tests_properties(base.result PROPERTIES TIMEOUT 10)
    add_executable(siftwing_checked_test tests/unit/checked_test.cc)
    target_link_libraries(siftwing_checked_test PRIVATE siftwing_base)
    add_test(NAME base.checked COMMAND siftwing_checked_test)
    set_tests_properties(base.checked PROPERTIES TIMEOUT 30)
    add_executable(siftwing_id_test tests/unit/id_test.cc)
    target_link_libraries(siftwing_id_test PRIVATE siftwing_base)
    add_test(NAME base.id COMMAND siftwing_id_test)
    set_tests_properties(base.id PROPERTIES TIMEOUT 10)
    add_executable(siftwing_document_record_test tests/unit/document_record_test.cc)
    target_link_libraries(siftwing_document_record_test PRIVATE siftwing_document)
    add_test(NAME document.record COMMAND siftwing_document_record_test)

    add_executable(siftwing_record_format_test tests/unit/record_format_test.cc)
    target_link_libraries(siftwing_record_format_test PRIVATE siftwing_record_format)
    add_test(NAME document.record_format COMMAND siftwing_record_format_test)
    add_executable(siftwing_record_format_probe tests/integration/record_format_probe.cc)
    target_link_libraries(siftwing_record_format_probe PRIVATE siftwing_record_format)
    add_test(NAME document.record_format_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/record_format_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_record_format_probe>")
    set_tests_properties(document.record_format document.record_format_reference PROPERTIES TIMEOUT 30)
    add_executable(siftwing_document_store_test tests/unit/document_store_test.cc)
    target_link_libraries(siftwing_document_store_test PRIVATE siftwing_document_store)
    target_link_options(siftwing_document_store_test PRIVATE "-Wl,--wrap=pread")
    add_test(NAME document.store COMMAND siftwing_document_store_test "${CMAKE_CURRENT_BINARY_DIR}")
    add_executable(siftwing_document_store_probe tests/integration/document_store_probe.cc)
    target_link_libraries(siftwing_document_store_probe PRIVATE siftwing_document_store)
    add_test(NAME document.store_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/document_store_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_document_store_probe>")
    set_tests_properties(document.store document.store_reference PROPERTIES TIMEOUT 30)
    set_tests_properties(document.record PROPERTIES TIMEOUT 10)
    add_executable(siftwing_document_reader_test tests/unit/document_reader_test.cc)
    target_link_libraries(siftwing_document_reader_test PRIVATE siftwing_document_reader)
    target_compile_definitions(siftwing_document_reader_test PRIVATE
        SIFTWING_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/golden")
    add_test(NAME document.reader COMMAND siftwing_document_reader_test)
    set_tests_properties(document.reader PROPERTIES TIMEOUT 10)
    add_executable(siftwing_txt_reader_test tests/unit/txt_reader_test.cc)
    target_link_libraries(siftwing_txt_reader_test PRIVATE siftwing_txt_reader)
    add_test(NAME document.txt_reader COMMAND siftwing_txt_reader_test)
    set_tests_properties(document.txt_reader PROPERTIES TIMEOUT 15)
    # 显式文件接入验收探针；公开CTest只生成原创输入，不依赖私有语料。
    add_executable(siftwing_txt_corpus_probe tests/integration/txt_corpus_probe.cc)
    target_link_libraries(siftwing_txt_corpus_probe PRIVATE siftwing_txt_reader)
    add_test(NAME document.txt_file_ingestion
        COMMAND Python3::Interpreter -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/txt_file_ingestion_test.py"
            --probe "$<TARGET_FILE:siftwing_txt_corpus_probe>")
    set_tests_properties(document.txt_file_ingestion PROPERTIES TIMEOUT 60)
    add_executable(siftwing_html_text_test tests/unit/html_text_test.cc)
    target_link_libraries(siftwing_html_text_test PRIVATE siftwing_html_text)
    add_test(NAME document.html_text COMMAND siftwing_html_text_test)
    set_tests_properties(document.html_text PROPERTIES TIMEOUT 20)
    add_executable(siftwing_html_fragment_probe tests/integration/html_fragment_probe.cc)
    target_link_libraries(siftwing_html_fragment_probe PRIVATE siftwing_html_text)
    add_test(NAME document.html_fragment_integration
        COMMAND Python3::Interpreter -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/html_fragment_integration_test.py"
            --probe "$<TARGET_FILE:siftwing_html_fragment_probe>"
            --rss "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/golden/rss/original.xml")
    set_tests_properties(document.html_fragment_integration PROPERTIES TIMEOUT 30)
    add_executable(siftwing_rss_reader_test tests/unit/rss_reader_test.cc)
    target_link_libraries(siftwing_rss_reader_test PRIVATE siftwing_rss_reader)
    add_test(NAME document.rss_reader COMMAND siftwing_rss_reader_test)
    set_tests_properties(document.rss_reader PROPERTIES TIMEOUT 30)
    add_executable(siftwing_rss_corpus_probe tests/integration/rss_corpus_probe.cc)
    target_link_libraries(siftwing_rss_corpus_probe PRIVATE siftwing_rss_reader)
    add_test(NAME document.rss_ingestion
        COMMAND Python3::Interpreter -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/rss_ingestion_test.py"
            --probe "$<TARGET_FILE:siftwing_rss_corpus_probe>"
            --rss "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/golden/rss/original.xml")
    set_tests_properties(document.rss_ingestion PROPERTIES TIMEOUT 30)
    add_executable(siftwing_normalize_test tests/unit/normalize_test.cc)
    target_link_libraries(siftwing_normalize_test PRIVATE siftwing_text_normalization)
    add_test(NAME text.normalization COMMAND siftwing_normalize_test)
    set_tests_properties(text.normalization PROPERTIES TIMEOUT 15)
    add_executable(siftwing_normalize_probe tests/integration/normalize_probe.cc)
    target_link_libraries(siftwing_normalize_probe PRIVATE siftwing_text_normalization)
    add_test(NAME text.normalization_reference
        COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/normalize_reference_test.py"
            --probe "$<TARGET_FILE:siftwing_normalize_probe>")
    set_tests_properties(text.normalization_reference PROPERTIES TIMEOUT 60)
    add_executable(siftwing_normalization_ingestion_test tests/integration/text_normalization_ingestion_test.cc)
    target_link_libraries(siftwing_normalization_ingestion_test PRIVATE
        siftwing_text_normalization siftwing_txt_reader siftwing_rss_reader)
    add_test(NAME text.normalization_ingestion COMMAND siftwing_normalization_ingestion_test)
    set_tests_properties(text.normalization_ingestion PROPERTIES TIMEOUT 15)
    add_executable(siftwing_english_tokenizer_test tests/unit/english_tokenizer_test.cc)
    target_link_libraries(siftwing_english_tokenizer_test PRIVATE siftwing_tokenizer)
    add_test(NAME text.english_tokenizer COMMAND siftwing_english_tokenizer_test)
    set_tests_properties(text.english_tokenizer PROPERTIES TIMEOUT 15)
    add_executable(siftwing_english_tokenizer_probe tests/integration/english_tokenizer_probe.cc)
    target_link_libraries(siftwing_english_tokenizer_probe PRIVATE siftwing_tokenizer)
    add_test(NAME text.english_tokenizer_reference
        COMMAND Python3::Interpreter -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/english_tokenizer_reference_test.py"
            --probe "$<TARGET_FILE:siftwing_english_tokenizer_probe>")
    set_tests_properties(text.english_tokenizer_reference PROPERTIES TIMEOUT 60)
    add_executable(siftwing_english_tokenizer_ingestion_test tests/integration/english_tokenizer_ingestion_test.cc)
    target_link_libraries(siftwing_english_tokenizer_ingestion_test PRIVATE
        siftwing_tokenizer siftwing_text_normalization siftwing_txt_reader siftwing_rss_reader)
    add_test(NAME text.english_tokenizer_ingestion COMMAND siftwing_english_tokenizer_ingestion_test)
    set_tests_properties(text.english_tokenizer_ingestion PROPERTIES TIMEOUT 15)
    add_executable(siftwing_cppjieba_tokenizer_test tests/unit/cppjieba_tokenizer_test.cc)
    target_link_libraries(siftwing_cppjieba_tokenizer_test PRIVATE siftwing_cppjieba_tokenizer)
    add_test(NAME text.cppjieba_tokenizer COMMAND siftwing_cppjieba_tokenizer_test)
    add_executable(siftwing_cppjieba_tokenizer_probe tests/integration/cppjieba_tokenizer_probe.cc)
    target_link_libraries(siftwing_cppjieba_tokenizer_probe PRIVATE siftwing_cppjieba_tokenizer)
    add_test(NAME text.cppjieba_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/cppjieba_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_cppjieba_tokenizer_probe>")
    add_executable(siftwing_cppjieba_ingestion_test tests/integration/cppjieba_tokenizer_ingestion_test.cc)
    target_link_libraries(siftwing_cppjieba_ingestion_test PRIVATE siftwing_cppjieba_tokenizer
        siftwing_text_normalization siftwing_txt_reader siftwing_rss_reader)
    add_test(NAME text.cppjieba_ingestion COMMAND siftwing_cppjieba_ingestion_test)
    set_tests_properties(text.cppjieba_tokenizer text.cppjieba_reference text.cppjieba_ingestion PROPERTIES TIMEOUT 60)
    add_executable(siftwing_term_frequency_test tests/unit/term_frequency_test.cc)
    target_link_libraries(siftwing_term_frequency_test PRIVATE siftwing_term_frequency)
    add_test(NAME text.term_frequency COMMAND siftwing_term_frequency_test)
    add_executable(siftwing_term_frequency_probe tests/integration/term_frequency_probe.cc)
    target_link_libraries(siftwing_term_frequency_probe PRIVATE siftwing_term_frequency)
    add_test(NAME text.term_frequency_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/term_frequency_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_term_frequency_probe>")
    add_executable(siftwing_term_frequency_ingestion_test tests/integration/term_frequency_ingestion_test.cc)
    target_link_libraries(siftwing_term_frequency_ingestion_test PRIVATE siftwing_term_frequency
        siftwing_cppjieba_tokenizer siftwing_text_normalization siftwing_txt_reader siftwing_rss_reader)
    add_test(NAME text.term_frequency_ingestion COMMAND siftwing_term_frequency_ingestion_test)
    set_tests_properties(text.term_frequency text.term_frequency_reference text.term_frequency_ingestion PROPERTIES TIMEOUT 60)
    add_executable(siftwing_lexicon_test tests/unit/lexicon_test.cc)
    target_link_libraries(siftwing_lexicon_test PRIVATE siftwing_lexicon)
    add_test(NAME index.lexicon COMMAND siftwing_lexicon_test)
    add_executable(siftwing_lexicon_probe tests/integration/lexicon_probe.cc)
    target_link_libraries(siftwing_lexicon_probe PRIVATE siftwing_lexicon)
    add_test(NAME index.lexicon_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/lexicon_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_lexicon_probe>")
    add_executable(siftwing_lexicon_ingestion_test tests/integration/lexicon_ingestion_test.cc)
    target_link_libraries(siftwing_lexicon_ingestion_test PRIVATE siftwing_lexicon
        siftwing_text_pipeline siftwing_duplicate_decision)
    add_test(NAME index.lexicon_ingestion COMMAND siftwing_lexicon_ingestion_test)
    set_tests_properties(index.lexicon index.lexicon_reference index.lexicon_ingestion PROPERTIES TIMEOUT 30)
    add_executable(siftwing_posting_test tests/unit/posting_test.cc)
    target_link_libraries(siftwing_posting_test PRIVATE siftwing_posting)
    add_test(NAME index.posting COMMAND siftwing_posting_test)
    add_executable(siftwing_posting_ingestion_test tests/integration/posting_ingestion_test.cc)
    target_link_libraries(siftwing_posting_ingestion_test PRIVATE siftwing_posting siftwing_term_frequency)
    add_test(NAME index.posting_ingestion COMMAND siftwing_posting_ingestion_test)
    set_tests_properties(index.posting index.posting_ingestion PROPERTIES TIMEOUT 15)
    add_executable(siftwing_inverted_index_test tests/unit/inverted_index_test.cc)
    target_link_libraries(siftwing_inverted_index_test PRIVATE siftwing_inverted_index)
    add_test(NAME index.inverted_index COMMAND siftwing_inverted_index_test)
    add_executable(siftwing_inverted_index_probe tests/integration/inverted_index_probe.cc)
    target_link_libraries(siftwing_inverted_index_probe PRIVATE siftwing_inverted_index)
    add_test(NAME index.inverted_index_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/inverted_index_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_inverted_index_probe>")
    add_executable(siftwing_inverted_index_ingestion_test tests/integration/inverted_index_ingestion_test.cc)
    target_link_libraries(siftwing_inverted_index_ingestion_test PRIVATE siftwing_inverted_index
        siftwing_text_pipeline siftwing_tokenizer siftwing_duplicate_decision)
    add_test(NAME index.inverted_index_ingestion COMMAND siftwing_inverted_index_ingestion_test)
    add_test(NAME index.inverted_index_compile_contract COMMAND "${CMAKE_COMMAND}"
        "-DCXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/inverted-index-contract"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/inverted_index_compile_contract.cmake")
    set_tests_properties(index.inverted_index index.inverted_index_reference
        index.inverted_index_ingestion index.inverted_index_compile_contract PROPERTIES TIMEOUT 60)
    add_executable(siftwing_weighted_index_test tests/unit/weighted_index_test.cc)
    target_link_libraries(siftwing_weighted_index_test PRIVATE siftwing_weights)
    add_test(NAME index.weights COMMAND siftwing_weighted_index_test)
    add_executable(siftwing_weighted_index_probe tests/integration/weighted_index_probe.cc)
    target_link_libraries(siftwing_weighted_index_probe PRIVATE siftwing_weights)
    add_test(NAME index.weights_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/weighted_index_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_weighted_index_probe>")
    add_executable(siftwing_weighted_index_ingestion_test tests/integration/weighted_index_ingestion_test.cc)
    target_link_libraries(siftwing_weighted_index_ingestion_test PRIVATE siftwing_weights
        siftwing_text_pipeline siftwing_tokenizer siftwing_duplicate_decision)
    add_test(NAME index.weights_ingestion COMMAND siftwing_weighted_index_ingestion_test)
    add_test(NAME index.weights_compile_contract COMMAND "${CMAKE_COMMAND}"
        "-DCXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/weights-contract"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/weighted_index_compile_contract.cmake")
    set_tests_properties(index.weights index.weights_reference index.weights_ingestion
        index.weights_compile_contract PROPERTIES TIMEOUT 60)
    add_executable(siftwing_manifest_test tests/unit/manifest_test.cc)
    target_link_libraries(siftwing_manifest_test PRIVATE siftwing_snapshot_manifest)
    add_test(NAME snapshot.manifest COMMAND siftwing_manifest_test)
    add_executable(siftwing_manifest_probe tests/integration/manifest_probe.cc)
    target_link_libraries(siftwing_manifest_probe PRIVATE siftwing_snapshot_manifest)
    add_test(NAME snapshot.manifest_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/manifest_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_manifest_probe>")
    add_executable(siftwing_manifest_ingestion_test tests/integration/manifest_ingestion_test.cc)
    target_link_libraries(siftwing_manifest_ingestion_test PRIVATE siftwing_snapshot_manifest
        siftwing_weights siftwing_document_store siftwing_text_pipeline siftwing_tokenizer)
    add_test(NAME snapshot.manifest_ingestion COMMAND siftwing_manifest_ingestion_test)
    add_test(NAME snapshot.manifest_compile_contract COMMAND "${CMAKE_COMMAND}"
        "-DCXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/manifest-contract"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/manifest_compile_contract.cmake")
    set_tests_properties(snapshot.manifest snapshot.manifest_reference snapshot.manifest_ingestion
        snapshot.manifest_compile_contract PROPERTIES TIMEOUT 60)
    add_executable(siftwing_writer_test tests/unit/writer_test.cc)
    target_link_libraries(siftwing_writer_test PRIVATE siftwing_snapshot_writer)
    foreach(operation openat write pread getrandom mkdirat fsync close fstat fdopendir renameat2)
        target_link_options(siftwing_writer_test PRIVATE "-Wl,--wrap=${operation}")
    endforeach()
    add_test(NAME snapshot.writer COMMAND siftwing_writer_test "${CMAKE_CURRENT_BINARY_DIR}")
    add_executable(siftwing_writer_probe tests/integration/writer_probe.cc)
    target_link_libraries(siftwing_writer_probe PRIVATE siftwing_snapshot_writer)
    add_test(NAME snapshot.writer_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/writer_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_writer_probe>" --parent "${CMAKE_CURRENT_BINARY_DIR}")
    add_executable(siftwing_writer_race_test tests/integration/writer_race_test.cc)
    target_link_libraries(siftwing_writer_race_test PRIVATE siftwing_snapshot_writer)
    target_link_options(siftwing_writer_race_test PRIVATE "-Wl,--wrap=renameat2")
    add_test(NAME snapshot.writer_race COMMAND siftwing_writer_race_test "${CMAKE_CURRENT_BINARY_DIR}")
    add_executable(siftwing_writer_ingestion_test tests/integration/writer_ingestion_test.cc)
    target_link_libraries(siftwing_writer_ingestion_test PRIVATE siftwing_snapshot_writer
        siftwing_text_pipeline siftwing_tokenizer)
    add_test(NAME snapshot.writer_ingestion COMMAND siftwing_writer_ingestion_test "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME snapshot.writer_compile_contract COMMAND "${CMAKE_COMMAND}"
        "-DCXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/writer-contract"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/writer_compile_contract.cmake")
    set_tests_properties(snapshot.writer snapshot.writer_reference snapshot.writer_race
        snapshot.writer_ingestion snapshot.writer_compile_contract PROPERTIES TIMEOUT 60)
    add_executable(siftwing_snapshot_manager_test tests/integration/snapshot_manager_test.cc)
    target_link_libraries(siftwing_snapshot_manager_test PRIVATE siftwing_snapshot_manager Threads::Threads)
    target_link_options(siftwing_snapshot_manager_test PRIVATE "-Wl,--wrap=_Znwm"
        "-Wl,--wrap=_ZN8siftwing8snapshot14SnapshotReader4openERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEENS_4base6detail8StrongIdINSB_18SnapshotVersionTagEEERKNS0_18SnapshotReadLimitsE")
    add_test(NAME snapshot.manager COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/snapshot_manager_test.py"
        --native "$<TARGET_FILE:siftwing_snapshot_manager_test>" --parent "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(snapshot.manager PROPERTIES TIMEOUT 180)
    add_executable(siftwing_snapshot_loader_test tests/integration/snapshot_loader_test.cc)
    target_link_libraries(siftwing_snapshot_loader_test PRIVATE siftwing_snapshot_loader)
    target_link_options(siftwing_snapshot_loader_test PRIVATE "-Wl,--wrap=_Znwm"
        "-Wl,--wrap=_ZN8siftwing8snapshot14SnapshotReader4openERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEENS_4base6detail8StrongIdINSB_18SnapshotVersionTagEEERKNS0_18SnapshotReadLimitsE")
    add_test(NAME snapshot.loader COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/snapshot_loader_test.py"
        --native "$<TARGET_FILE:siftwing_snapshot_loader_test>" --parent "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(snapshot.loader PROPERTIES TIMEOUT 180)
    add_executable(siftwing_snapshot_publication_test tests/integration/snapshot_publication_test.cc)
    target_link_libraries(siftwing_snapshot_publication_test PRIVATE siftwing_online_server siftwing_json)
    target_link_options(siftwing_snapshot_publication_test PRIVATE "-Wl,--wrap=_Znwm"
        "-Wl,--wrap=_ZNK8siftwing8snapshot15SnapshotManager7currentEv"
        "-Wl,--wrap=_ZNK8siftwing5query13SearchService7executeB5cxx11ERKNS_11concurrency10SearchTaskEPNS0_16CacheObservationEPSt8optionalImE")
    add_test(NAME snapshot.publication COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/snapshot_publication_test.py"
        --native "$<TARGET_FILE:siftwing_snapshot_publication_test>" --parent "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_options(siftwing_snapshot_publication_test PRIVATE
        "-Wl,--wrap=_ZN8siftwing8snapshot15SnapshotManager14load_candidateERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEENS_4base6detail8StrongIdINSB_18SnapshotVersionTagEEERKNS0_18SnapshotReadLimitsE"
        "-Wl,--wrap=_ZN8siftwing8snapshot14SnapshotLoader4joinEv"
        "-Wl,--wrap=_ZN8siftwing11concurrency10WorkerPool12request_stopEv")
    add_test(NAME net.signalfd_online COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/snapshot_publication_test.py"
        --native "$<TARGET_FILE:siftwing_snapshot_publication_test>" --parent "${CMAKE_CURRENT_BINARY_DIR}" --signals)
    set_tests_properties(snapshot.publication net.signalfd_online PROPERTIES TIMEOUT 180)
    # V3连续故障/真实fd耗尽与有界持续运行；沿用生产接口，无测试hook。
    add_executable(siftwing_v3_fault_soak_test tests/integration/v3_fault_soak_test.cc)
    target_link_libraries(siftwing_v3_fault_soak_test PRIVATE siftwing_online_server siftwing_json)
    add_test(NAME v3.fault_recovery COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/v3_fault_soak_test.py"
        --native "$<TARGET_FILE:siftwing_v3_fault_soak_test>" --parent "${CMAKE_CURRENT_BINARY_DIR}" --suite faults)
    foreach(subs IN ITEMS 1 2)
        add_test(NAME v3.soak_${subs} COMMAND Python3::Interpreter -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/v3_fault_soak_test.py"
            --native "$<TARGET_FILE:siftwing_v3_fault_soak_test>" --parent "${CMAKE_CURRENT_BINARY_DIR}"
            --suite soak --subs ${subs} --seconds 2)
        set_tests_properties(v3.soak_${subs} PROPERTIES TIMEOUT 120 LABELS "v3;soak")
    endforeach()
    add_test(NAME v3.shutdown_races COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/snapshot_publication_test.py"
        --native "$<TARGET_FILE:siftwing_snapshot_publication_test>" --parent "${CMAKE_CURRENT_BINARY_DIR}" --races)
    set_tests_properties(v3.fault_recovery v3.shutdown_races PROPERTIES TIMEOUT 180 LABELS "v3;fault")
    add_executable(siftwing_reader_test tests/unit/reader_test.cc)
    target_link_libraries(siftwing_reader_test PRIVATE siftwing_snapshot_reader siftwing_snapshot_writer)
    foreach(operation openat pread fstat fstatat close fdopendir readdir closedir)
        target_link_options(siftwing_reader_test PRIVATE "-Wl,--wrap=${operation}")
    endforeach()
    add_test(NAME snapshot.reader COMMAND siftwing_reader_test "${CMAKE_CURRENT_BINARY_DIR}")
    add_executable(siftwing_reader_probe tests/integration/reader_probe.cc)
    target_link_libraries(siftwing_reader_probe PRIVATE siftwing_snapshot_reader)
    add_test(NAME snapshot.reader_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/reader_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_reader_probe>" --parent "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME snapshot.reader_compile_contract COMMAND "${CMAKE_COMMAND}"
        "-DCXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/reader-contract"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/reader_compile_contract.cmake")
    set_tests_properties(snapshot.reader snapshot.reader_reference snapshot.reader_compile_contract PROPERTIES TIMEOUT 60)
    add_executable(siftwing_roundtrip_probe tests/integration/roundtrip_probe.cc)
    target_link_libraries(siftwing_roundtrip_probe PRIVATE siftwing_snapshot_reader siftwing_snapshot_writer)
    add_executable(siftwing_roundtrip_pipeline_test tests/integration/roundtrip_pipeline_test.cc)
    target_link_libraries(siftwing_roundtrip_pipeline_test PRIVATE siftwing_snapshot_reader siftwing_snapshot_writer
        siftwing_txt_reader siftwing_rss_reader siftwing_text_pipeline siftwing_tokenizer siftwing_duplicate_decision)
    add_test(NAME snapshot.roundtrip COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/roundtrip_test.py"
        --writer "$<TARGET_FILE:siftwing_writer_probe>" --reader "$<TARGET_FILE:siftwing_reader_probe>"
        --republish "$<TARGET_FILE:siftwing_roundtrip_probe>" --pipeline "$<TARGET_FILE:siftwing_roundtrip_pipeline_test>"
        --parent "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(snapshot.roundtrip PROPERTIES TIMEOUT 120)
    add_executable(siftwing_query_test tests/unit/query_test.cc)
    target_link_libraries(siftwing_query_test PRIVATE siftwing_query siftwing_tokenizer)
    add_test(NAME query.model COMMAND siftwing_query_test)
    add_executable(siftwing_query_probe tests/integration/query_probe.cc)
    target_link_libraries(siftwing_query_probe PRIVATE siftwing_query siftwing_tokenizer)
    add_test(NAME query.reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/query_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_query_probe>")
    add_executable(siftwing_query_ingestion_test tests/integration/query_ingestion_test.cc)
    target_link_libraries(siftwing_query_ingestion_test PRIVATE siftwing_query siftwing_tokenizer
        siftwing_cppjieba_tokenizer siftwing_txt_reader siftwing_rss_reader)
    add_test(NAME query.ingestion COMMAND siftwing_query_ingestion_test)
    add_test(NAME query.compile_contract COMMAND "${CMAKE_COMMAND}"
        "-DCXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/query-contract"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/query_compile_contract.cmake")
    set_tests_properties(query.model query.reference query.ingestion query.compile_contract PROPERTIES TIMEOUT 60)
    add_executable(siftwing_candidates_test tests/unit/candidates_test.cc)
    target_link_libraries(siftwing_candidates_test PRIVATE siftwing_candidates siftwing_tokenizer)
    add_test(NAME query.candidates COMMAND siftwing_candidates_test)
    add_executable(siftwing_candidates_probe tests/integration/candidates_probe.cc)
    target_link_libraries(siftwing_candidates_probe PRIVATE siftwing_candidates siftwing_tokenizer)
    add_test(NAME query.candidates_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/candidates_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_candidates_probe>")
    add_executable(siftwing_candidates_snapshot_test tests/integration/candidates_snapshot_test.cc)
    target_link_libraries(siftwing_candidates_snapshot_test PRIVATE siftwing_candidates siftwing_tokenizer
        siftwing_cppjieba_tokenizer siftwing_txt_reader siftwing_rss_reader
        siftwing_snapshot_writer siftwing_snapshot_reader)
    add_test(NAME query.candidates_snapshot COMMAND siftwing_candidates_snapshot_test "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME query.candidates_compile_contract COMMAND "${CMAKE_COMMAND}"
        "-DCXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/candidates-contract"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/candidates_compile_contract.cmake")
    set_tests_properties(query.candidates query.candidates_reference query.candidates_snapshot
        query.candidates_compile_contract PROPERTIES TIMEOUT 60)
    add_executable(siftwing_ranking_test tests/unit/ranking_test.cc)
    target_link_libraries(siftwing_ranking_test PRIVATE siftwing_ranking siftwing_tokenizer)
    add_test(NAME query.ranking COMMAND siftwing_ranking_test)
    add_executable(siftwing_ranking_probe tests/integration/ranking_probe.cc)
    target_link_libraries(siftwing_ranking_probe PRIVATE siftwing_ranking siftwing_tokenizer)
    add_test(NAME query.ranking_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/ranking_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_ranking_probe>")
    add_executable(siftwing_ranking_snapshot_test tests/integration/ranking_snapshot_test.cc)
    target_link_libraries(siftwing_ranking_snapshot_test PRIVATE siftwing_ranking siftwing_tokenizer
        siftwing_cppjieba_tokenizer siftwing_txt_reader siftwing_rss_reader
        siftwing_snapshot_writer siftwing_snapshot_reader)
    add_test(NAME query.ranking_snapshot COMMAND siftwing_ranking_snapshot_test "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME query.ranking_compile_contract COMMAND "${CMAKE_COMMAND}"
        "-DCXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/ranking-contract"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/ranking_compile_contract.cmake")
    set_tests_properties(query.ranking query.ranking_reference query.ranking_snapshot
        query.ranking_compile_contract PROPERTIES TIMEOUT 60)
    add_executable(siftwing_summary_test tests/unit/summary_test.cc)
    target_link_libraries(siftwing_summary_test PRIVATE siftwing_summary)
    add_test(NAME query.summary COMMAND siftwing_summary_test)
    add_executable(siftwing_summary_probe tests/integration/summary_probe.cc)
    target_link_libraries(siftwing_summary_probe PRIVATE siftwing_summary)
    add_test(NAME query.summary_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/summary_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_summary_probe>")
    add_executable(siftwing_summary_snapshot_test tests/integration/summary_snapshot_test.cc)
    target_link_libraries(siftwing_summary_snapshot_test PRIVATE siftwing_summary siftwing_top_k
        siftwing_snapshot_writer siftwing_snapshot_reader siftwing_txt_reader siftwing_rss_reader
        siftwing_cppjieba_tokenizer)
    add_test(NAME query.summary_snapshot COMMAND siftwing_summary_snapshot_test "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME query.summary_compile_contract COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/summary-compile-contract"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/summary_compile_contract.cmake")
    set_tests_properties(query.summary query.summary_reference query.summary_snapshot
        query.summary_compile_contract PROPERTIES TIMEOUT 60)

    add_executable(siftwing_recommendation_test tests/unit/recommendation_test.cc)
    target_link_libraries(siftwing_recommendation_test PRIVATE siftwing_recommendation)
    add_test(NAME query.recommendation COMMAND siftwing_recommendation_test)
    add_executable(siftwing_recommendation_probe tests/integration/recommendation_probe.cc)
    target_link_libraries(siftwing_recommendation_probe PRIVATE siftwing_recommendation)
    add_test(NAME query.recommendation_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/recommendation_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_recommendation_probe>")
    add_executable(siftwing_recommendation_snapshot_test tests/integration/recommendation_snapshot_test.cc)
    target_link_libraries(siftwing_recommendation_snapshot_test PRIVATE siftwing_recommendation siftwing_snapshot_reader
        siftwing_snapshot_writer siftwing_text_pipeline siftwing_cppjieba_tokenizer siftwing_txt_reader siftwing_rss_reader)
    add_test(NAME query.recommendation_snapshot COMMAND siftwing_recommendation_snapshot_test "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME query.recommendation_compile_contract COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/recommendation-compile-contract"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/recommendation_compile_contract.cmake")
    set_tests_properties(query.recommendation query.recommendation_reference query.recommendation_snapshot
        query.recommendation_compile_contract PROPERTIES TIMEOUT 60)

    add_executable(siftwing_highlight_test tests/unit/highlight_test.cc)
    target_link_libraries(siftwing_highlight_test PRIVATE siftwing_highlight)
    add_test(NAME query.highlight COMMAND siftwing_highlight_test)
    add_executable(siftwing_highlight_probe tests/integration/highlight_probe.cc)
    target_link_libraries(siftwing_highlight_probe PRIVATE siftwing_highlight)
    add_test(NAME query.highlight_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/highlight_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_highlight_probe>")
    add_executable(siftwing_highlight_snapshot_test tests/integration/highlight_snapshot_test.cc)
    target_link_libraries(siftwing_highlight_snapshot_test PRIVATE siftwing_highlight siftwing_summary siftwing_top_k
        siftwing_snapshot_writer siftwing_snapshot_reader siftwing_txt_reader siftwing_rss_reader
        siftwing_cppjieba_tokenizer)
    add_test(NAME query.highlight_snapshot COMMAND siftwing_highlight_snapshot_test "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME query.highlight_compile_contract COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/highlight-compile-contract"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/highlight_compile_contract.cmake")
    set_tests_properties(query.highlight query.highlight_reference query.highlight_snapshot
        query.highlight_compile_contract PROPERTIES TIMEOUT 60)

    add_executable(siftwing_top_k_test tests/unit/top_k_test.cc)
    target_link_libraries(siftwing_top_k_test PRIVATE siftwing_top_k siftwing_tokenizer)
    add_test(NAME query.top_k COMMAND siftwing_top_k_test)
    add_executable(siftwing_top_k_probe tests/integration/top_k_probe.cc)
    target_link_libraries(siftwing_top_k_probe PRIVATE siftwing_top_k siftwing_tokenizer)
    add_test(NAME query.top_k_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/top_k_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_top_k_probe>")
    add_executable(siftwing_top_k_snapshot_test tests/integration/top_k_snapshot_test.cc)
    target_link_libraries(siftwing_top_k_snapshot_test PRIVATE siftwing_top_k siftwing_tokenizer
        siftwing_cppjieba_tokenizer siftwing_txt_reader siftwing_rss_reader
        siftwing_snapshot_writer siftwing_snapshot_reader)
    add_test(NAME query.top_k_snapshot COMMAND siftwing_top_k_snapshot_test "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME query.top_k_compile_contract COMMAND "${CMAKE_COMMAND}"
        "-DCXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/top-k-contract"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/top_k_compile_contract.cmake")
    set_tests_properties(query.top_k query.top_k_reference query.top_k_snapshot
        query.top_k_compile_contract PROPERTIES TIMEOUT 60)
    add_executable(siftwing_text_pipeline_test tests/unit/text_pipeline_test.cc)
    target_link_libraries(siftwing_text_pipeline_test PRIVATE siftwing_text_pipeline)
    add_test(NAME text.pipeline COMMAND siftwing_text_pipeline_test)
    add_executable(siftwing_text_pipeline_ingestion_test tests/integration/text_pipeline_ingestion_test.cc)
    target_link_libraries(siftwing_text_pipeline_ingestion_test PRIVATE siftwing_text_pipeline
        siftwing_cppjieba_tokenizer siftwing_txt_reader siftwing_rss_reader)
    add_test(NAME text.pipeline_ingestion COMMAND siftwing_text_pipeline_ingestion_test)
    set_tests_properties(text.pipeline text.pipeline_ingestion PROPERTIES TIMEOUT 60)
    add_executable(siftwing_text_pipeline_probe tests/integration/text_pipeline_probe.cc)
    target_link_libraries(siftwing_text_pipeline_probe PRIVATE siftwing_text_pipeline siftwing_cppjieba_tokenizer)
    add_test(NAME text.pipeline_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/text_pipeline_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_text_pipeline_probe>")
    set_tests_properties(text.pipeline_reference PROPERTIES TIMEOUT 120)
    add_executable(siftwing_simhash_test tests/unit/simhash_test.cc)
    target_link_libraries(siftwing_simhash_test PRIVATE siftwing_simhash)
    add_test(NAME document.simhash COMMAND siftwing_simhash_test)
    add_executable(siftwing_simhash_ingestion_test tests/integration/simhash_ingestion_test.cc)
    target_link_libraries(siftwing_simhash_ingestion_test PRIVATE siftwing_simhash
        siftwing_text_pipeline siftwing_cppjieba_tokenizer siftwing_txt_reader siftwing_rss_reader)
    add_test(NAME document.simhash_ingestion COMMAND siftwing_simhash_ingestion_test)
    add_executable(siftwing_simhash_probe tests/integration/simhash_probe.cc)
    target_link_libraries(siftwing_simhash_probe PRIVATE siftwing_simhash)
    add_test(NAME document.simhash_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/simhash_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_simhash_probe>")
    set_tests_properties(document.simhash document.simhash_ingestion document.simhash_reference PROPERTIES TIMEOUT 30)
    add_executable(siftwing_duplicate_decision_test tests/unit/duplicate_decision_test.cc)
    target_link_libraries(siftwing_duplicate_decision_test PRIVATE siftwing_duplicate_decision)
    add_test(NAME document.duplicate_decision COMMAND siftwing_duplicate_decision_test)
    add_executable(siftwing_duplicate_decision_ingestion_test tests/integration/duplicate_decision_ingestion_test.cc)
    target_link_libraries(siftwing_duplicate_decision_ingestion_test PRIVATE siftwing_duplicate_decision
        siftwing_text_pipeline siftwing_cppjieba_tokenizer siftwing_txt_reader siftwing_rss_reader)
    add_test(NAME document.duplicate_ingestion COMMAND siftwing_duplicate_decision_ingestion_test)
    add_executable(siftwing_duplicate_decision_probe tests/integration/duplicate_decision_probe.cc)
    target_link_libraries(siftwing_duplicate_decision_probe PRIVATE siftwing_duplicate_decision)
    add_test(NAME document.duplicate_reference COMMAND Python3::Interpreter -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/duplicate_decision_reference_test.py"
        --probe "$<TARGET_FILE:siftwing_duplicate_decision_probe>")
    set_tests_properties(document.duplicate_decision document.duplicate_ingestion document.duplicate_reference PROPERTIES TIMEOUT 30)
    if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
        add_test(NAME index.posting_compile_contract
            COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/posting-compile-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/posting_compile_contract.cmake")
        set_tests_properties(index.posting_compile_contract PROPERTIES TIMEOUT 30)
        add_test(NAME index.lexicon_compile_contract
            COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/lexicon-compile-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/lexicon_compile_contract.cmake")
        set_tests_properties(index.lexicon_compile_contract PROPERTIES TIMEOUT 30)
        add_test(NAME document.store_compile_contract
            COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/document-store-compile-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/document_store_compile_contract.cmake")
        set_tests_properties(document.store_compile_contract PROPERTIES TIMEOUT 30)
        add_test(NAME document.duplicate_compile_contract
            COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/duplicate-compile-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/duplicate_decision_compile_contract.cmake")
        set_tests_properties(document.duplicate_compile_contract PROPERTIES TIMEOUT 30)
        add_test(NAME document.simhash_compile_contract
            COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/simhash-compile-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/simhash_compile_contract.cmake")
        set_tests_properties(document.simhash_compile_contract PROPERTIES TIMEOUT 30)
        add_test(NAME text.pipeline_compile_contract
            COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/text-pipeline-compile-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/text_pipeline_compile_contract.cmake")
        set_tests_properties(text.pipeline_compile_contract PROPERTIES TIMEOUT 30)
        add_test(NAME text.term_frequency_compile_contract
            COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/term-frequency-compile-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/term_frequency_compile_contract.cmake")
        set_tests_properties(text.term_frequency_compile_contract PROPERTIES TIMEOUT 30)
        add_test(NAME text.cppjieba_compile_contract
            COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/cppjieba-compile-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/cppjieba_compile_contract.cmake")
        set_tests_properties(text.cppjieba_compile_contract PROPERTIES TIMEOUT 30)
        add_test(NAME text.tokenizer_compile_contract
            COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/tokenizer-compile-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/tokenizer_compile_contract.cmake")
        set_tests_properties(text.tokenizer_compile_contract PROPERTIES TIMEOUT 30)
        add_test(NAME text.normalization_compile_contract
            COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/normalization-compile-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/normalize_compile_contract.cmake")
        set_tests_properties(text.normalization_compile_contract PROPERTIES TIMEOUT 30)
        add_test(NAME document.rss_reader_compile_contract
            COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/rss-reader-compile-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/rss_reader_compile_contract.cmake")
        set_tests_properties(document.rss_reader_compile_contract PROPERTIES TIMEOUT 30)
        add_test(NAME document.html_text_compile_contract
            COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/html-text-compile-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/html_text_compile_contract.cmake")
        set_tests_properties(document.html_text_compile_contract PROPERTIES TIMEOUT 30)
        add_test(NAME document.txt_reader_compile_contract
            COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/txt-reader-compile-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/txt_reader_compile_contract.cmake")
        set_tests_properties(document.txt_reader_compile_contract PROPERTIES TIMEOUT 30)
        add_test(NAME document.reader_compile_contract
            COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/document-reader-compile-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/document_reader_compile_contract.cmake")
        set_tests_properties(document.reader_compile_contract PROPERTIES TIMEOUT 30)
        add_test(NAME base.result_compile_contract
            COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/result-compile-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/result_compile_contract.cmake")
        set_tests_properties(base.result_compile_contract PROPERTIES TIMEOUT 30)
        add_test(NAME base.checked_compile_contract
            COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/checked-compile-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/checked_compile_contract.cmake")
        set_tests_properties(base.checked_compile_contract PROPERTIES TIMEOUT 30)
        add_test(NAME base.id_compile_contract
            COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/id-compile-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/id_compile_contract.cmake")
        set_tests_properties(base.id_compile_contract PROPERTIES TIMEOUT 60)
        add_test(NAME document.record_compile_contract
            COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/document-record-compile-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/document_record_compile_contract.cmake")
        set_tests_properties(document.record_compile_contract PROPERTIES TIMEOUT 30)
    endif()
endif()

if(BUILD_TESTING)
    # 复用独立CLI行为参考，分别覆盖纯V1入口的正常与拒绝路径。
    foreach(suite IN ITEMS integration failures)
        add_test(NAME app.v1_cli_${suite} COMMAND Python3::Interpreter -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/cli_test.py"
            --cli "$<TARGET_FILE:siftwing_v1_cli>" --indexer "$<TARGET_FILE:siftwing_indexer>"
            --writer "$<TARGET_FILE:siftwing_writer_probe>" --suite "${suite}")
        set_tests_properties(app.v1_cli_${suite} PROPERTIES TIMEOUT 120)
    endforeach()
endif()
