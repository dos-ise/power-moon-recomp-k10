#!/bin/bash
# The NVIDIA DLSS runtime the Windows release ships next to the exe (loaded by the DLSS probe when the menu enables
# it), fetched from NVIDIA's own repository at a pinned revision into build/upscalers/, checked against its sha256
# and its Authenticode signature (NVIDIA's), with the licence it comes under:
# - NVIDIA DLSS 310.9.1: nvngx_dlss.dll - github.com/NVIDIA/DLSS (NVIDIA RTX SDKs licence)
# make dist copies it (and the licence: tools/dist_licenses.sh). Skipped for a file already there with its hash.
set -euo pipefail
cd "$(dirname "$0")/.."
O=build/upscalers
mkdir -p "$O"

DLSS=https://github.com/NVIDIA/DLSS/raw/v310.9.1

signer() {  # file -> the subject of its valid Authenticode signature, or nothing
    powershell.exe -NoProfile -Command "\$s = Get-AuthenticodeSignature -LiteralPath '$(cygpath -w "$1")'; if (\$s.Status -eq 'Valid') { \$s.SignerCertificate.Subject }" | tr -d '\r'
}

get() {  # url sha256 file signer-pattern (empty: a text file, no signature)
    local f="$O/$3"
    if [ ! -f "$f" ] || [ "$(sha256sum "$f" | cut -d' ' -f1)" != "$2" ]; then
        curl -fsSL --proto '=https' -o "$f.part" "$1"
        [ "$(sha256sum "$f.part" | cut -d' ' -f1)" = "$2" ] || { echo "$3: sha256 differs from the pinned one"; rm -f "$f.part"; exit 1; }
        mv "$f.part" "$f"
    fi
    if [ -n "$4" ]; then
        local s; s=$(signer "$f")
        [[ "$s" == *"$4"* ]] || { echo "$3: not signed by $4 (signature: ${s:-none})"; exit 1; }
    fi
}

get "$DLSS/lib/Windows_x86_64/rel/nvngx_dlss.dll" 3975567b8943c53acce397f2b72380092f84f162d00b0d2c7d08a1025c563983 \
    nvngx_dlss.dll "NVIDIA Corporation"
get "$DLSS/LICENSE.txt" "$(sha256sum third_party/dlss/LICENSE.txt | cut -d' ' -f1)" NVIDIA-RTX-SDK-LICENSE.txt ""
# NVIDIA Streamline 2.14.16 (DLSS frame generation, Reflex: runtime/nvn/vulkan/sl_glue.cpp): its runtime DLLs from the
# SDK release zip (pinned), each checked for NVIDIA's signature.
SL_DLLS="sl.interposer.dll sl.common.dll sl.dlss_g.dll sl.reflex.dll sl.pcl.dll nvngx_dlssg.dll NvLowLatencyVk.dll"
need=0
for f in $SL_DLLS; do [ -f "$O/$f" ] || need=1; done
if [ $need = 1 ]; then
    Z="$O/streamline-sdk.zip"
    curl -fsSL --proto '=https' -o "$Z.part" \
        https://github.com/NVIDIAGameWorks/Streamline/releases/download/v2.14.16/streamline-sdk-v2.14.16.zip
    [ "$(sha256sum "$Z.part" | cut -d' ' -f1)" = 32041e51db0cab709a271b72a352281b690c2abbd669a827809401a5dd23956b ] ||
        { echo "streamline-sdk.zip: sha256 differs from the pinned one"; rm -f "$Z.part"; exit 1; }
    mv "$Z.part" "$Z"
    for f in $SL_DLLS; do unzip -o -j -q "$Z" "bin/x64/$f" -d "$O"; done
    unzip -o -j -q "$Z" "bin/x64/nvngx_dlss.license.txt" "bin/x64/reflex.license.txt" -d "$O"
    rm -f "$Z"
fi
for f in $SL_DLLS; do
    s=$(signer "$O/$f")
    [[ "$s" == *"NVIDIA Corporation"* ]] || { echo "$f: not signed by NVIDIA (signature: ${s:-none})"; exit 1; }
done
echo "upscalers: $(ls "$O" | tr '\n' ' ')"
