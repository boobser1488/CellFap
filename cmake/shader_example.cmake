# Script mode: the example shader mod's cell.frag = the game's cell.frag with main() renamed cellfapBaseMain(),
# followed by tint.glsl (sdk/examples/shader_tint). Called with -DSRC=... -DTINT=... -DOUT=...
file(READ ${SRC} base)
file(READ ${TINT} tint)
string(REPLACE "void main()" "void cellfapBaseMain()" base "${base}")
file(WRITE ${OUT}.tmp "${base}${tint}")
configure_file(${OUT}.tmp ${OUT} COPYONLY)
file(REMOVE ${OUT}.tmp)
