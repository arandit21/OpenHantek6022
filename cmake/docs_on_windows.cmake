if (NOT WIN32)
    return()
endif()

# Windows portable layout: English documents at root, Russian at /ru.
set(SRC_DOCS_DIR "${CMAKE_CURRENT_LIST_DIR}/../docs")
set(TARGET_DOCUMENTS "documents")

add_custom_command(TARGET ${PROJECT_NAME}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>/${TARGET_DOCUMENTS}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>/${TARGET_DOCUMENTS}/ru"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SRC_DOCS_DIR}/HANTEK6022_AC_Modification.pdf" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/${TARGET_DOCUMENTS}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SRC_DOCS_DIR}/HANTEK6022_Frequency_Generator_Modification.pdf" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/${TARGET_DOCUMENTS}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SRC_DOCS_DIR}/OpenHantek6022_User_Manual.pdf" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/${TARGET_DOCUMENTS}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SRC_DOCS_DIR}/OpenHantek6022_zadig_Win10.pdf" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/${TARGET_DOCUMENTS}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SRC_DOCS_DIR}/ru/HANTEK6022_AC_Modification.pdf" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/${TARGET_DOCUMENTS}/ru"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SRC_DOCS_DIR}/ru/HANTEK6022_Frequency_Generator_Modification.pdf" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/${TARGET_DOCUMENTS}/ru"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SRC_DOCS_DIR}/ru/OpenHantek6022_User_Manual.pdf" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/${TARGET_DOCUMENTS}/ru"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SRC_DOCS_DIR}/ru/OpenHantek6022_zadig_Win10.pdf" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/${TARGET_DOCUMENTS}/ru"
    COMMENT "Copy English and Russian PDF documentation for ${PROJECT_NAME}"
)
