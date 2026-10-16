# SPDX-License-Identifier: MIT
# Runs after every build of the bundles (the squatch-finish-bundles target):
# - puts clap-wrapper's generated AU Info.plist (with its AudioComponents entry) back into each
#   .component. Re-running CMake's configure step regenerates a bundle's default Info.plist in
#   place, and clap-wrapper only copies its own when the AU target relinks, so an up-to-date AU
#   could be left unregistrable ("no types found" in pluginval, nothing in auval -a).
# - signs every bundle ad hoc, so the signature seals the final Info.plist.
#   cmake -DBUNDLES=<dir> -DBUILD=<dir> -DPLUGINS="key=Name|key=Name" -P finish-bundles.cmake
string(REPLACE "|" ";" plugins "${PLUGINS}")
foreach(pair IN LISTS plugins)
  string(FIND "${pair}" "=" at)
  string(SUBSTRING "${pair}" 0 ${at} key)
  math(EXPR from "${at} + 1")
  string(SUBSTRING "${pair}" ${from} -1 name)
  set(au "${BUNDLES}/${name}.component")
  set(plist "${BUILD}/squatch-${key}_auv2-build-helper-output/auv2_Info.plist")
  if(EXISTS "${au}" AND EXISTS "${plist}")
    file(COPY_FILE "${plist}" "${au}/Contents/Info.plist")
  endif()
  foreach(ext clap vst3 component)
    if(EXISTS "${BUNDLES}/${name}.${ext}")
      execute_process(COMMAND codesign --force --sign - "${BUNDLES}/${name}.${ext}" RESULT_VARIABLE failed OUTPUT_QUIET ERROR_QUIET)
      if(failed)
        message(FATAL_ERROR "codesign failed for ${name}.${ext}")
      endif()
    endif()
  endforeach()
endforeach()
