function(add_wav_io_test_module)
    set(LIB_NAME "wav_io")
    
    add_library(${LIB_NAME} STATIC 
        "${PROJECT_SOURCE_DIR}/common_audio/wav_file.cc"
    )

    # 设置该库所需的头文件路径
    target_include_directories(${LIB_NAME} PUBLIC 
        "${PROJECT_SOURCE_DIR}/"
    )

    # --- 2. 定义测试可执行程序 ---
    set(TEST_NAME "test_wav_io")
    
    add_executable(${TEST_NAME} "${PROJECT_SOURCE_DIR}/test/test_wav_io.cc")


    target_link_libraries(${TEST_NAME} PRIVATE 
        ${LIB_NAME}
    )

    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endfunction()