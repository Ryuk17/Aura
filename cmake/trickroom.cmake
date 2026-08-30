set(TRICKROOM_VERSION "v0.1.0")
set(TRICKROOM_URL "https://github.com/Ryuk17/TrickRoom/archive/refs/tags/release.tar.gz")
set(URL_HASH "SHA256:a4a5182301fac3789d8fefea7a1ee8bf5db0bc383b3f5e68167f57d09377e557")


FetchContent_Declare(TRICKROOM
	URL ${TRICKROOM_URL}
	URL_HASH ${URL_HASH} 
)
FetchContent_MakeAvailable(TRICKROOM)
include_directories(${TRICKROOM_SOURCE_DIR}/include)
link_directories(${TRICKROOM_SOURCE_DIR}/lib)
