find_package(Qt6 REQUIRED COMPONENTS Core Gui Sql)

add_executable(ExperimentStorageSmoke
    experiment_storage_smoke.cpp
    ../openhantek/src/experiment/experimentcatalog.cpp
    ../openhantek/src/experiment/experimentcatalog.h
    ../openhantek/src/experiment/experimentmanager.cpp
    ../openhantek/src/experiment/experimentmanager.h
    ../openhantek/src/experiment/rawrecordingengine.cpp
    ../openhantek/src/experiment/rawrecordingengine.h
    ../openhantek/src/experiment/rawplaybackengine.cpp
    ../openhantek/src/experiment/rawplaybackengine.h
    ../openhantek/src/post/processor.cpp
    ../openhantek/src/post/ppresult.cpp
    ../openhantek/src/post/ppresult.h
)
if (MSVC)
    target_sources(ExperimentStorageSmoke PRIVATE
        ../openhantek/src/experiment/mp4videorecorder.cpp
        ../openhantek/src/experiment/mp4videorecorder.h
    )
    target_compile_definitions(ExperimentStorageSmoke PRIVATE OPENHANTEK_TEST_MP4)
    target_link_libraries(ExperimentStorageSmoke PRIVATE mfplat mfreadwrite mfuuid ole32)
endif()
set_target_properties(ExperimentStorageSmoke PROPERTIES AUTOMOC ON)
target_include_directories(ExperimentStorageSmoke PRIVATE ../openhantek/src)
target_link_libraries(ExperimentStorageSmoke PRIVATE Qt6::Core Qt6::Gui Qt6::Sql)
target_compile_features(ExperimentStorageSmoke PRIVATE cxx_std_17)
add_test(NAME experiment-storage-regression COMMAND ExperimentStorageSmoke)
