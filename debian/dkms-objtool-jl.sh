#!/bin/sh
# Run objtool --hacks=jump_label on nvidia.ko and nvidia-modeset.ko
# Called from MAKE[0] in dkms.conf after "make modules" completes.
#
# Usage: dkms-objtool-jl.sh <kernel_source_dir>
#
# thunk-Kbuild.patch disables objtool on nvidia.o/nvidia-modeset.o to avoid
# validation errors from pre-compiled blobs. On kernels with delay-objtool
# (CONFIG_KLP_BUILD=y), this also skips the --hacks=jump_label pass that
# converts static_branch JMPs to NOPs. Without this conversion, toggling
# any static key that touches nvidia-modeset (e.g. freezer_active during
# suspend) triggers: kernel BUG at arch/x86/kernel/jump_label.c:73

OBJTOOL="$1/tools/objtool/objtool"

if [ ! -x "$OBJTOOL" ]; then
    echo "  OBJTOOL: not found, skipping jump_label fix"
    exit 0
fi

for mod in nvidia.ko nvidia-modeset.ko; do
    if [ -f "$mod" ]; then
        "$OBJTOOL" --hacks=jump_label --module --link "$mod"
        echo "  OBJTOOL [jump_label] $mod"
    fi
done
