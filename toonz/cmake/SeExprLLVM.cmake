# Optional, explicitly selected JIT backend. The interpreter remains available
# and is the application default in every build.
option(OT_SEEXPR_WITH_LLVM "Build the qualified SeExpr LLVM 14 backend" OFF)
set(OT_SEEXPR_HAS_LLVM 0)
set(_ot_seexpr_llvm_prepare_args)
if(OT_SEEXPR_WITH_LLVM)
  find_package(LLVM 14 REQUIRED CONFIG)
  if(NOT LLVM_VERSION_MAJOR EQUAL 14)
    message(FATAL_ERROR "The SeExpr JIT is qualified against LLVM 14; select LLVM_DIR for that version")
  endif()
  set(OT_SEEXPR_HAS_LLVM 1)
  list(APPEND _ot_seexpr_llvm_prepare_args --llvm)
endif()

function(groundzero_seexpr_configure_llvm target)
  target_compile_definitions(${target} PUBLIC
    OT_SEEXPR_HAS_LLVM=${OT_SEEXPR_HAS_LLVM})
  if(NOT OT_SEEXPR_WITH_LLVM)
    return()
  endif()
  target_include_directories(${target} SYSTEM PUBLIC ${LLVM_INCLUDE_DIRS})
  separate_arguments(_llvm_definitions NATIVE_COMMAND "${LLVM_DEFINITIONS}")
  target_compile_options(${target} PRIVATE ${_llvm_definitions})
  # Prefer the shared distro LLVM target so packages do not acquire multiple
  # copies of global LLVM registries. Static SDK distributions use components.
  if(TARGET LLVM)
    target_link_libraries(${target} PRIVATE LLVM)
  else()
    llvm_map_components_to_libnames(_llvm_libraries
      core executionengine mcjit native nativecodegen ipo scalaropts
      instcombine transformutils analysis target passes support)
    target_link_libraries(${target} PRIVATE ${_llvm_libraries})
  endif()
endfunction()
