# The example modification (mods/example) built into the game as gen/example_mod.inc: the game writes it into its mods
# folder on the first start, so the phone gets it too. Re-generated whenever one of its files changes.
function(cellfap_example_mod SRC_DIR OUT_FILE)
    file(GLOB_RECURSE files RELATIVE ${SRC_DIR} ${SRC_DIR}/*)
    list(SORT files)
    set(content "// Generated from mods/example by cmake/example_mod.cmake. Do not edit.\n")
    foreach(f ${files})
        file(READ ${SRC_DIR}/${f} text)
        string(APPEND content "{\"${f}\", R\"cfmod(${text})cfmod\"},\n")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SRC_DIR}/${f})
    endforeach()
    file(WRITE ${OUT_FILE}.tmp "${content}")
    configure_file(${OUT_FILE}.tmp ${OUT_FILE} COPYONLY)  # touched only when it changed
endfunction()
