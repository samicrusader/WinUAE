#!/bin/bash
# Configure and build the static FFmpeg that WinUAE links. Run through build.cmd,
# which sets up the MSVC environment and the vcpkg dependencies first.
#   build.sh [x86|x64]
# Component policy (user, 2026-10-25; PrevueWork/shared/ffmpeg.md): decode only;
# media archival, streaming rips, tape dumps, internet files; live input over
# network protocols. No Blu-ray, DVD-Video, CDDA, voice synthesis, game music,
# GSM, ModPlug, VANC, SMB, capture devices, hardware decoders.
set -e
ARCH=${1:-x86}
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$HERE/src
DEPS=$HERE/deps/$ARCH-windows-static
OUT=$HERE/$ARCH
BUILD=$HERE/obj-$ARCH
case $ARCH in x86) FFARCH=x86 ;; x64) FFARCH=x86_64 ;; *) echo "bad arch $ARCH"; exit 1 ;; esac

[ -f "$SRC/configure" ] || { echo "no FFmpeg source in $SRC (git submodule update --init)"; exit 1; }
[ -d "$DEPS/lib" ] || { echo "no vcpkg deps in $DEPS"; exit 1; }
command -v cl >/dev/null || { echo "cl.exe not on PATH (run build.cmd)"; exit 1; }
command -v pkgconf >/dev/null || command -v pkg-config >/dev/null || { echo "MSYS2 needs pkgconf: pacman -S pkgconf"; exit 1; }

# MSYS2 ships a link.exe of its own (coreutils); make sure MSVC's wins.
export PATH="$(dirname "$(command -v cl)"):$PATH"
# vcpkg's libjxl port names a library it doesn't install (jxl-static.lib; the file is jxl.lib)
sed -i 's/-ljxl-static\b/-ljxl/' "$DEPS/lib/pkgconfig/libjxl.pc"

export PKG_CONFIG_PATH="$DEPS/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$DEPS/lib/pkgconfig"

join() { local IFS=,; echo "$*"; }

VDEC=(
  # modern / internet
  h264 hevc vvc vp8 vp9 libdav1d apv theora vp3 vp4 vp5 vp6 vp6a vp6f vp7 snow dirac
  mpeg1video mpeg2video mpeg4 msmpeg4v1 msmpeg4v2 msmpeg4v3 h261 h263 h263i h263p flv
  vc1 vc1image wmv1 wmv2 wmv3 wmv3image rv10 rv20 rv30 rv40 rv60 svq1 svq3
  cinepak indeo2 indeo3 indeo4 indeo5 qtrle rpza smc msrle msvideo1 aasc tscc tscc2 zmbv
  # archival / tape / intermediate
  ffv1 ffvhuff huffyuv utvideo magicyuv lagarith cllc sheervideo v210 v210x r210 r10k
  y41p yuv4 rawvideo prores prores_raw dnxhd dvvideo cfhd hap mszh zlib fraps fic
  # stills
  mjpeg mjpegb jpegls jpeg2000 libjxl libjxl_anim png apng webp gif bmp tiff targa
  qoi exr hdr psd dds pcx sgi sgirle sunrast ppm pgm pgmyuv pbm pam xbm xpm xwd wbmp
  ptx pictor vbn txd photocd
)
ADEC=(
  aac aac_fixed aac_latm ac3 ac3_fixed eac3 dca truehd mlp s302m dolby_e dvaudio
  mp1 mp1float mp2 mp2float mp3 mp3float mp3adu mp3adufloat mp3on4 mp3on4float
  opus vorbis flac alac als wavpack ape tak tta
  wmav1 wmav2 wmapro wmalossless cook sipr ra_144 ra_288 atrac1 atrac3 atrac3al atrac3p atrac3pal
  amrnb amrwb nellymoser qdm2 qdmc mace3 mace6 imc
  adpcm_ima_wav adpcm_ima_qt adpcm_ms adpcm_swf adpcm_yamaha
  pcm_alaw pcm_mulaw pcm_bluray pcm_dvd pcm_lxf pcm_s24daud
  pcm_f16le pcm_f24le pcm_f32be pcm_f32le pcm_f64be pcm_f64le
  pcm_s8 pcm_s8_planar pcm_u8 pcm_s16be pcm_s16be_planar pcm_s16le pcm_s16le_planar
  pcm_s24be pcm_s24le pcm_s24le_planar pcm_s32be pcm_s32le pcm_s32le_planar pcm_s64be pcm_s64le
  pcm_u16be pcm_u16le pcm_u24be pcm_u24le pcm_u32be pcm_u32le
  dsd_lsbf dsd_lsbf_planar dsd_msbf dsd_msbf_planar
)
DEMUX=(
  mov matroska avi asf mpegts mpegtsraw mpegps mpegvideo m4v h261 h263 h264 hevc vvc
  vc1 vc1t ivf obu av1 apv dirac dnxhd cavsvideo flv live_flv swf rm ogg nut mxf gxf lxf dv
  wtv ty nuv pva nsv smjpeg mjpeg mjpeg_2000 mpjpeg yuv4mpegpipe rawvideo v210 v210x
  wav w64 aiff caf au flac mp3 aac ac3 eac3 dts dtshd truehd mlp loas wv ape tak tta
  amr amrnb amrwb s337m spdif shorten mpc mpc8 xwma
  pcm_alaw pcm_mulaw pcm_f32be pcm_f32le pcm_f64be pcm_f64le pcm_s8 pcm_u8
  pcm_s16be pcm_s16le pcm_s24be pcm_s24le pcm_s32be pcm_s32le
  pcm_u16be pcm_u16le pcm_u24be pcm_u24le pcm_u32be pcm_u32le
  image2 image2pipe apng gif webp_anim jpegxl_anim
  image_bmp_pipe image_dds_pipe image_dpx_pipe image_exr_pipe image_gif_pipe image_hdr_pipe
  image_j2k_pipe image_jpeg_pipe image_jpegls_pipe image_jpegxl_pipe image_pam_pipe
  image_pbm_pipe image_pcx_pipe image_pgm_pipe image_pgmyuv_pipe image_photocd_pipe
  image_pictor_pipe image_png_pipe image_ppm_pipe image_psd_pipe image_qoi_pipe
  image_sgi_pipe image_sunrast_pipe image_tiff_pipe image_vbn_pipe image_webp_pipe
  image_xbm_pipe image_xpm_pipe image_xwd_pipe
  # live / network
  hls dash rtsp rtp sdp concat
)
PARSE=(
  aac aac_latm ac3 apv av1 bmp cavsvideo cook dca dirac dnxhd dpx dvaudio flac gif h261 h263
  h264 hevc vvc jpeg2000 jpegxl mjpeg mlp mpeg4video mpegaudio mpegvideo opus png pnm prores
  qoi rv34 tak vc1 vorbis vp3 vp8 vp9 webp xbm xwd
)
PROTO=(
  file pipe fd cache async concat concatf crypto data subfile
  tcp udp udplite rtp srtp tls http https httpproxy icecast ftp mmsh mmst
  rtmp rtmps rtmpt rtmpts ffrtmphttp libsrt
)

mkdir -p "$BUILD" "$OUT"
cd "$BUILD"
if [ ! -f config.h ] || [ "$HERE/build.sh" -nt config.h ]; then
  "$SRC/configure" \
    --prefix="$OUT" --toolchain=msvc --arch=$FFARCH --target-os=win32 \
    --enable-static --disable-shared --pkg-config-flags=--static \
    --extra-cflags="-MT -I$DEPS/include" --extra-ldflags="-LIBPATH:$DEPS/lib" \
    --enable-gpl --enable-version3 \
    --disable-autodetect --enable-w32threads --enable-schannel --enable-zlib \
    --enable-libdav1d --enable-libjxl --enable-libxml2 --enable-libsrt \
    --disable-programs --disable-doc --disable-avdevice --disable-debug \
    --disable-everything \
    --enable-decoder="$(join "${VDEC[@]}" "${ADEC[@]}")" \
    --enable-demuxer="$(join "${DEMUX[@]}")" \
    --enable-parser="$(join "${PARSE[@]}")" \
    --enable-protocol="$(join "${PROTO[@]}")" \
    --enable-filters --disable-filter=dnn_classify,dnn_detect,dnn_processing \
    --enable-bsfs \
    || { tail -40 ffbuild/config.log; exit 1; }
fi
make -j"$(nproc)"
make install
# Record what was built against, so a stale tree is obvious.
( cd "$SRC" && git rev-parse HEAD 2>/dev/null || echo unknown ) > "$OUT/FFMPEG_COMMIT"
cp ffbuild/config.log "$OUT/config.log" 2>/dev/null || true
grep -h '^#define CONFIG_.*_\(DECODER\|DEMUXER\|PROTOCOL\|FILTER\) 1' config_components.h | sed 's/#define CONFIG_//; s/ 1$//' > "$OUT/COMPONENTS"
echo "built FFmpeg $ARCH into $OUT"
