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

file(GLOB_RECURSE maa_ios_control_unit_src *.h *.hpp *.cpp)
file(GLOB_RECURSE maa_ios_control_unit_header ${MAA_PUBLIC_INC}/MaaControlUnit/IOSControlUnitAPI.h
                                              ${MAA_PUBLIC_INC}/MaaControlUnit/ControlUnitAPI.h)

# scrctl 的 src/app/ 是它的命令行程序（带 main()），不能进库。
file(GLOB_RECURSE scrctl_core_src ${SCRCTL_SOURCE_DIR}/src/*.cpp ${SCRCTL_SOURCE_DIR}/src/*.h)
list(FILTER scrctl_core_src EXCLUDE REGEX ".*/src/app/.*")

add_library(MaaIOSControlUnit SHARED
    ${maa_ios_control_unit_src}
    ${maa_ios_control_unit_header}
    ${scrctl_core_src})

find_package(OpenSSL REQUIRED)

target_include_directories(MaaIOSControlUnit
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${SCRCTL_SOURCE_DIR}/src ${MAA_PRIVATE_INC} ${MAA_PUBLIC_INC})

target_link_libraries(MaaIOSControlUnit PRIVATE MaaUtils ${OpenCV_LIBS} OpenSSL::SSL OpenSSL::Crypto)

# scrctl 的解码后端要这三个框架（照 scrctl 自己的 CMakeLists 来，不多链一个）。
find_library(CORE_FOUNDATION CoreFoundation REQUIRED)
find_library(VIDEO_TOOLBOX VideoToolbox REQUIRED)
find_library(CORE_MEDIA CoreMedia REQUIRED)
find_library(CORE_VIDEO CoreVideo REQUIRED)

target_link_libraries(MaaIOSControlUnit PRIVATE
    ${CORE_FOUNDATION}
    ${VIDEO_TOOLBOX}
    ${CORE_MEDIA}
    ${CORE_VIDEO})

target_compile_definitions(MaaIOSControlUnit PRIVATE MAA_CONTROL_UNIT_EXPORTS)

add_dependencies(MaaIOSControlUnit MaaUtils)

install(
    TARGETS MaaIOSControlUnit
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION bin
    # ARCHIVE DESTINATION lib
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${maa_ios_control_unit_src})
source_group(TREE ${SCRCTL_SOURCE_DIR}/src PREFIX "scrctl" FILES ${scrctl_core_src})
