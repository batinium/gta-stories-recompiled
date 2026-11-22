# [android] libadrenotools (third_party/libadrenotools, BSD-2-Clause, with linkernsbypass, BSD-2-Clause): loads a Vulkan
# driver the player imported (custom GPU drivers, docs/ANDROID.md; src/android/gpu_driver.cpp). Our own targets over the
# vendored sources (its CMakeLists.txt is kept as it came, unused): a static library linked into vcs_rhi, and the hook
# libraries, shared, which Gradle packages beside libmain.so - adrenotools loads them from nativeLibraryDir
# (useLegacyPackaging is on, android/app/build.gradle). The library ships no driver; nothing here is a driver.
set(_at "${CMAKE_SOURCE_DIR}/third_party/libadrenotools")

add_library(vcs_linkernsbypass STATIC "${_at}/lib/linkernsbypass/android_linker_ns.cpp"
                                      "${_at}/lib/linkernsbypass/elf_soname_patcher.cpp")
target_include_directories(vcs_linkernsbypass PUBLIC "${_at}/lib/linkernsbypass")
target_link_libraries(vcs_linkernsbypass PUBLIC android dl)

# driver.cpp only (adrenotools_open_libvulkan and friends); bcenabler is not needed
add_library(vcs_adrenotools STATIC "${_at}/src/driver.cpp")
target_include_directories(vcs_adrenotools PUBLIC "${_at}/include" PRIVATE "${_at}" "${_at}/src")
target_link_libraries(vcs_adrenotools PUBLIC vcs_linkernsbypass android log)

# The hooks: names fixed by adrenotools (libhook_impl.so, libmain_hook.so, libfile_redirect_hook.so, libgsl_alloc_hook.so)
add_library(vcs_at_hook_impl SHARED "${_at}/src/hook/hook_impl.cpp")
target_include_directories(vcs_at_hook_impl PRIVATE "${_at}/include")
target_link_libraries(vcs_at_hook_impl PRIVATE vcs_linkernsbypass log)
set_target_properties(vcs_at_hook_impl PROPERTIES OUTPUT_NAME hook_impl CXX_VISIBILITY_PRESET hidden)
foreach(_hook main_hook file_redirect_hook gsl_alloc_hook)
    add_library(vcs_at_${_hook} SHARED "${_at}/src/hook/${_hook}.c")
    target_include_directories(vcs_at_${_hook} PRIVATE "${_at}/include")
    target_link_options(vcs_at_${_hook} PRIVATE "-Wl,-z,global")
    target_link_libraries(vcs_at_${_hook} PRIVATE vcs_at_hook_impl)
    set_target_properties(vcs_at_${_hook} PROPERTIES OUTPUT_NAME ${_hook})
endforeach()

foreach(_t vcs_linkernsbypass vcs_adrenotools vcs_at_hook_impl vcs_at_main_hook vcs_at_file_redirect_hook vcs_at_gsl_alloc_hook)
    set_target_properties(${_t} PROPERTIES POSITION_INDEPENDENT_CODE ON UNITY_BUILD OFF)
endforeach()
