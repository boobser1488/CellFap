# Встраивает исходники шейдеров в заголовочный файл, чтобы редактор шейдера в игре мог их показать и вернуть
# к исходному состоянию. Запуск: cmake -DINPUT=... -DOUTPUT=... -P embed_shaders.cmake
#
# Каждый шейдер становится строкой kShaderSrc<Имя>. Разделитель /* --- next --- */ не может встретиться внутри
# GLSL-исходника (это не корректный синтаксис GLSL), поэтому склейка однозначна.

file(GLOB SHADER_FILES "${INPUT}/*.vert" "${INPUT}/*.frag" "${INPUT}/*.glsl")
list(SORT SHADER_FILES)

set(OUT "// Сгенерировано из shaders/ — не править руками.\n#pragma once\n\n")
set(NAMES "")
foreach(f ${SHADER_FILES})
    get_filename_component(base "${f}" NAME)
    string(REPLACE "." "_" ident "${base}")
    file(READ "${f}" content)
    # Экранируем только то, что ломает raw string: закрывающий литерал и последовательность кавычек.
    string(REPLACE ")SHADER\"" ")" "content" "${content}")
    string(APPEND OUT "static const char* const kShaderSrc${ident} = R\"SHADER(${content})SHADER\";\n\n")
endforeach()

# Таблица: имя файла -> исходник. Имена нужны UI в том виде, в каком их показывают игроку.
set(ARR "")
foreach(f ${SHADER_FILES})
    get_filename_component(base "${f}" NAME)
    string(REPLACE "." "_" ident "${base}")
    string(APPEND ARR "    {\"${base}\", kShaderSrc${ident}},\n")
endforeach()
string(APPEND OUT "struct EmbeddedShader {\n    const char* name;\n    const char* source;\n};\n")
string(APPEND OUT "static const EmbeddedShader kShaderList[] = {\n${ARR}    };\n")
string(APPEND OUT "inline int embeddedShaderCount() { return (int)(sizeof(kShaderList) / sizeof(kShaderList[0])); }\n")
string(APPEND OUT "inline const EmbeddedShader* embeddedShaders() { return kShaderList; }\n")

file(WRITE "${OUTPUT}" "${OUT}")
