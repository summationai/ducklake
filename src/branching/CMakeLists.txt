add_library(
  ducklake_branching OBJECT
  ducklake_branch.cpp
  ducklake_branch_functions.cpp
  ducklake_branch_parser.cpp
  ducklake_branch_transaction.cpp
  ducklake_branching.cpp)

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:ducklake_branching>
    PARENT_SCOPE)
