# Standalone target added on top of the ytsaurus monorepo's own generated
# CMake build (see Dockerfile) -- not part of the upstream ya.make/CMake
# export, so it stays a plain hand-written CMakeLists.txt rather than the
# per-platform generated files the rest of the tree uses.
add_executable(httparena-yt-core-http main.cpp)

target_link_libraries(httparena-yt-core-http PRIVATE
  yt-core-http
  yt-core-https
  library-cpp-json
  build-cow-on
)

# Every ya.make PROGRAM() gets a generated version-info translation unit
# (GetProgramCommitId & co, referenced from yt-yt-core's build_info) for
# free; our own hand-added target needs to ask for the same thing explicitly.
vcs_info(httparena-yt-core-http)
