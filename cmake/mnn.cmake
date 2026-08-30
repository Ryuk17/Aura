set(MNN_VERSION "3.3.0")
set(MNN_URL "https://github.com/alibaba/MNN/archive/refs/tags/3.3.0.tar.gz")
set(URL_HASH "SHA256=8b45d905626742031c07f750acf5b790347a2dd76bfe76f775b8ca99172276c5")


FetchContent_Declare(MNN
	URL ${MNN_URL}
	URL_HASH ${URL_HASH} 
)
FetchContent_MakeAvailable(MNN)
include_directories(${MNN_SOURCE_DIR}/include)
link_directories(${MNN_SOURCE_DIR}/lib)
