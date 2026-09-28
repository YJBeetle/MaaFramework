# scrctl 的 C++ 核心**直接编进这个 dylib**。
#
# 为什么不依赖一个装好的 libscrctl：控制单元是被 MaaFramework dlopen 的独立库，
# 再多一个需要用户自己安装、还要保证 ABI 一致的动态依赖，只会把部署复杂化，
# 而 scrctl 现在也没有稳定 ABI 可言。代价是这个 target 编得慢一点。
#
# 位置：默认找同级目录的 scrctl（XCODECTRL/scrctl 与 XCODECTRL/MaaFramework 并排
# checkout 时的样子），不对再用 -DSCRCTL_SOURCE_DIR=... 指。
if(NOT SCRCTL_SOURCE_DIR)
    set(SCRCTL_SOURCE_DIR ${CMAKE_SOURCE_DIR}/../scrctl)
endif()

get_filename_component(SCRCTL_SOURCE_DIR ${SCRCTL_SOURCE_DIR} ABSOLUTE)

if(NOT EXISTS ${SCRCTL_SOURCE_DIR}/CMakeLists.txt)
    message(FATAL_ERROR
            "MaaIOSControlUnit 需要 scrctl 的源码，但 ${SCRCTL_SOURCE_DIR} 看起来不是 scrctl。"
            " clone https://github.com/YJBeetle/scrctl 到旁边，或配置时加"
            " -DSCRCTL_SOURCE_DIR=/path/to/scrctl")
endif()

file(GLOB_RECURSE maa_ios_control_unit_src CONFIGURE_DEPENDS *.h *.hpp *.cpp)
file(GLOB_RECURSE maa_ios_control_unit_header CONFIGURE_DEPENDS ${MAA_PUBLIC_INC}/MaaControlUnit/IOSControlUnitAPI.h
                                              ${MAA_PUBLIC_INC}/MaaControlUnit/ControlUnitAPI.h)

# CONFIGURE_DEPENDS 是必需的，不是锦上添花：scrctl 的源码是 glob 进来的，而 glob
# 只在配置期求值。scrctl 那边新增一个 .cpp 时，这里不会自动看见，症状是链接期报
# undefined symbol（真发生过：新增 src/rt/RtpSeq.cpp）。加了它，构建系统每次 build
# 前重新 glob，改了 scrctl 之后直接 cmake --build 就够，不用记得重跑 configure。

# scrctl 的 src/app/ 是它的命令行程序（带 main()），不能进库。
file(GLOB_RECURSE scrctl_core_src CONFIGURE_DEPENDS ${SCRCTL_SOURCE_DIR}/src/*.cpp ${SCRCTL_SOURCE_DIR}/src/*.h)
list(FILTER scrctl_core_src EXCLUDE REGEX ".*/src/app/.*")

# 解码后端按平台挑，视频和音频各一对，挑完每样只能有一个工厂函数的定义：
#   create_platform_decoder()  非 Apple 用 PlatformDecoderFallback.cpp / Apple 用 VideoToolboxDecoder.cpp
#   create_audio_decoder()     非 Apple 用 AudioDecoderNone.cpp      / Apple 用 AudioToolboxEldDecoder.cpp
# 一对里两个都在就是 duplicate symbol。规则要和 scrctl 自己 CMakeLists 的 target_sources
# 保持一致，它才是权威。
if(APPLE)
    list(FILTER scrctl_core_src EXCLUDE REGEX ".*/src/decode/PlatformDecoderFallback.cpp")
    list(FILTER scrctl_core_src EXCLUDE REGEX ".*/src/decode/AudioDecoderNone.cpp")
else()
    list(FILTER scrctl_core_src EXCLUDE REGEX ".*/src/decode/VideoToolboxDecoder.cpp")
    list(FILTER scrctl_core_src EXCLUDE REGEX ".*/src/decode/AudioToolboxEldDecoder.cpp")
endif()

# 软解后端（libavcodec）**故意不编进来**：这个 dylib 要能被 dlopen 就走，多一个
# 用户得自己装的动态依赖只会把部署搞复杂，而 scrctl 那边也还没有稳定 ABI。
# 代价要写清楚——没有软解时，关键帧超过 65535 字节的码流（真机主屏实测有）在
# VideoToolbox 上解不了，MaaFramework 侧的截图会一直拿不到画面。要补的话就在这里
# 用 pkg-config 找 libavcodec 并把它链上。
list(FILTER scrctl_core_src EXCLUDE REGEX ".*/src/decode/FFmpegDecoder.cpp")

add_library(MaaIOSControlUnit SHARED
    ${maa_ios_control_unit_src}
    ${maa_ios_control_unit_header}
    ${scrctl_core_src})

find_package(OpenSSL REQUIRED)

target_include_directories(MaaIOSControlUnit
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${SCRCTL_SOURCE_DIR}/src ${MAA_PRIVATE_INC} ${MAA_PUBLIC_INC})

# scrctl 的头必须排在依赖之前。它自己的 include 写成 "json/Json.h" 这种短路径，
# 而 macOS 默认文件系统大小写不敏感——一旦搜索路径上有个第三方 json/json.h
# （brew 的 jsoncpp 就是），scrctl 的头会被它顶掉，报出来的错看着像 scrctl 自己
# 坏了。scrctl 侧已把该模块改名成 jsonlite 消掉这个坑，这里保留顺序作为第二道防线。

target_link_libraries(MaaIOSControlUnit PRIVATE MaaUtils ${OpenCV_LIBS} OpenSSL::SSL OpenSSL::Crypto)

# scrctl 的解码后端要这几个框架（照 scrctl 自己的 CMakeLists 来，不多链一个）。
find_library(CORE_FOUNDATION CoreFoundation REQUIRED)
find_library(VIDEO_TOOLBOX VideoToolbox REQUIRED)
find_library(CORE_MEDIA CoreMedia REQUIRED)
find_library(CORE_VIDEO CoreVideo REQUIRED)
# AudioToolboxEldDecoder 用的是 AudioConverter*，在 AudioToolbox 里；它和
# VideoToolboxDecoder 一样是"按平台挑出来的那一个后端"，所以 Apple 上必须链。
find_library(AUDIO_TOOLBOX AudioToolbox REQUIRED)

target_link_libraries(MaaIOSControlUnit PRIVATE
    ${CORE_FOUNDATION}
    ${VIDEO_TOOLBOX}
    ${CORE_MEDIA}
    ${CORE_VIDEO}
    ${AUDIO_TOOLBOX})

target_compile_definitions(MaaIOSControlUnit PRIVATE MAA_CONTROL_UNIT_EXPORTS)

# OpenSSL 3 把 SHA512_Init/Update/Final 这类"一把梭"的摘要接口标成了
# deprecated，而 scrctl 的 SRP（无线配对那一步的数学层，src/wifi/Srp.cpp）用的
# 正是它们——接口在 3.x 里仍然稳定可用，只是官方推荐改走 EVP_MD_CTX。
#
# 为什么要在这里压掉而不是去改 scrctl：这个 target 带 -Werror，而 scrctl 自己的
# 构建不带，同一个警告在那边是提示、在这边是**编不出来**。MaaFW 的门槛不该由隔壁
# checkout 的告警策略决定，尤其这条还是"稳定但已废弃"而不是"错了"。
# 只压 OpenSSL 的废弃标记，别的其他 deprecation 照样红，所以盖不住自己的问题。
# 真要根治是让 scrctl 换 EVP 接口，换完之后这一行可以删。
target_compile_definitions(MaaIOSControlUnit PRIVATE OPENSSL_SUPPRESS_DEPRECATED)

add_dependencies(MaaIOSControlUnit MaaUtils)

install(
    TARGETS MaaIOSControlUnit
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION bin
    # ARCHIVE DESTINATION lib
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${maa_ios_control_unit_src})
source_group(TREE ${SCRCTL_SOURCE_DIR}/src PREFIX "scrctl" FILES ${scrctl_core_src})
