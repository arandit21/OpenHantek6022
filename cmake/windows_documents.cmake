# Windows documentation layout: documents/*.pdf (English), documents/ru/*.pdf (Russian).
# This file is retained for compatibility with alternate Windows build configurations.
set(OH_DOCS_DIR "${CMAKE_CURRENT_LIST_DIR}/../docs")
set(DOCUMENTS_DIR "documents")

add_custom_command(
    TARGET ${PROJECT_NAME}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>/${DOCUMENTS_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>/${DOCUMENTS_DIR}/ru"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OH_DOCS_DIR}/HANTEK6022_AC_Modification.pdf" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/${DOCUMENTS_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OH_DOCS_DIR}/HANTEK6022_Frequency_Generator_Modification.pdf" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/${DOCUMENTS_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OH_DOCS_DIR}/OpenHantek6022_User_Manual.pdf" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/${DOCUMENTS_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OH_DOCS_DIR}/OpenHantek6022_zadig_Win10.pdf" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/${DOCUMENTS_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OH_DOCS_DIR}/ru/HANTEK6022_AC_Modification.pdf" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/${DOCUMENTS_DIR}/ru"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OH_DOCS_DIR}/ru/HANTEK6022_Frequency_Generator_Modification.pdf" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/${DOCUMENTS_DIR}/ru"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OH_DOCS_DIR}/ru/OpenHantek6022_User_Manual.pdf" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/${DOCUMENTS_DIR}/ru"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OH_DOCS_DIR}/ru/OpenHantek6022_zadig_Win10.pdf" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/${DOCUMENTS_DIR}/ru"
    COMMENT "Copy English and Russian PDF documentation for ${PROJECT_NAME}"
)
