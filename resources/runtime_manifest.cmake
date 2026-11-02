# Post-build helper: writes qt.conf beside the executables and the private
# assembly manifest that lists every DLL windeployqt placed in RUNTIME_DIR.
# Usage: cmake -DBIN_DIR=<out/bin> -DRUNTIME_DIR=<out/bin/runtime> -P runtime_manifest.cmake
file(GLOB _dlls RELATIVE "${RUNTIME_DIR}" "${RUNTIME_DIR}/*.dll")
list(SORT _dlls)
set(_files "")
foreach(_dll IN LISTS _dlls)
    string(APPEND _files "  <file name=\"${_dll}\"/>\n")
endforeach()
file(WRITE "${RUNTIME_DIR}/runtime.manifest"
"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>
<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">
  <assemblyIdentity type=\"win32\" name=\"runtime\" version=\"1.0.0.0\" processorArchitecture=\"amd64\"/>
${_files}</assembly>
")
file(WRITE "${BIN_DIR}/qt.conf"
"[Paths]
Plugins = runtime
QmlImports = runtime/qml
")
