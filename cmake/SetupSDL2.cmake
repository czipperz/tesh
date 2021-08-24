# On Windows we download these manually.
if (WIN32)
    set(SDL2_DIR SDL)
    set(SDL2_IMAGE_PATH IMG)
    set(SDL2_TTF_PATH TTF)
endif()

find_package(SDL2 REQUIRED)
find_package(SDL2_image REQUIRED)
find_package(SDL2_ttf REQUIRED)

include_directories(${SDL2_INCLUDE_DIR} ${SDL2_IMAGE_INCLUDE_DIRS} ${SDL2_TTF_INCLUDE_DIRS})
link_libraries(${SDL2_LIBRARY} ${SDL2_IMAGE_LIBRARIES} ${SDL2_TTF_LIBRARIES})
