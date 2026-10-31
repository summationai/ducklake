add_library(
  ducklake_branching OBJECT
  ducklake_branch.cpp
  ducklake_branch_functions.cpp
  ducklake_branch_parser.cpp
  ducklake_branch_transaction.cpp
  ducklake_branching.cpp)

# Instruments the branching code only, for scripts/branching/coverage.sh
option(DUCKLAKE_BRANCHING_COVERAGE "Measure test coverage of the branching code" OFF)
if(DUCKLAKE_BRANCHING_COVERAGE)
  target_compile_options(ducklake_branching PRIVATE -fprofile-instr-generate
                                                    -fcoverage-mapping)
endif()

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:ducklake_branching>
    PARENT_SCOPE)
