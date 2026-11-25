#---------------------------------------------------------------------------------
# Card Wars -- Nintendo Switch wrapper (32-bit / AArch32, Unity 2022.3 Mono)
#
# Ships NO game code and NO game assets: the game's own APK (the player's
# copy, any name) is read at run time, and its libraries are unpacked from it
# on the first launch.
#
# The build is the android32 runtime's (runtime/runtime.mk: devkitARM +
# libnx32 + mesa32 from portlibs32/); ./build.sh runs it in the toolchain
# container. Output: cardwars_nx.nsp, which the launcher NRO carries (launcher/).
#---------------------------------------------------------------------------------
TARGET               := cardwars_nx
# one id per port (0x01000000000010xx); 1010-1013 and 1020 are taken by the
# other ports in this family -- change it if yours differs
PORT_NPDM_PROGRAM_ID := 0x0100000000001030
# 32-bit address space without the alias region (a8r does the same): the
# kernel gives the heap region more room, and the game had run out of 1 GiB
# (GPU buffers + Unity + Mono) at about frame 445.
PORT_NPDM_ADDRSPACE := 2
# More GL errors in the log (gl_mesa.c's RT_GL_CHECK, with the calls that raise
# them) for a bug report: -DRT_GL_CHECK=1 on the build's CFLAGS.
include runtime/runtime.mk

.PHONY: check
check:
	@echo "host-side checks (no toolchain needed):"
	@echo "  python3 tools/scan_imports.py <lib/armeabi-v7a>   # refresh tools/imports_needed.txt"
	@echo "  python3 runtime/tools/gen_imports.py --check      # every import has a shim?"
	@echo "  tools/hostcheck/run.sh                            # syntax-check source/ against the runtime headers"
