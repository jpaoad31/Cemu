import sys
out=sys.argv[1]
RGBA8='0x01a,0x41a'
groups=[
 # (comment, widths, heights, formats)
 ("TV and GamePad images (jet::draw::Renderer::AllocDisplayTarget: both 1280x720 RGBA8), the 3D scene and "
  "full-size post-effect buffers (RPGrpRenderer, ShaderSupport, DRCCamera, minigame targets), the display "
  "targets' depth buffer (D32F)",
  [1280],[720], RGBA8+',0x008,0x019,0x806,0x816,0x80e,0x011'),
 ("1/2 size: bloom/blur/glare buffers (ShaderSupport::InitializePostEffect 640x360 R16F/RGBA8/R5G6B5, "
  "RPGrpRenderer 640x360 RGBA4, minigame 640x360 targets). A render target is 640x368 (tiled height padding; "
  "368 isn't a multiple of 32, so Cemu keeps the padded size). Exact 640x360 leaves RGBA8 (0x01a) out: "
  "Common/RPStepUnicycle/common.carc has a 640x360 RGBA8 file texture, which would load blank if scaled",
  [640],[368], RGBA8+',0x008,0x00b,0x806'),
 ("1/2 size, exact 640x360 (no RGBA8, see above)",
  [640],[360], '0x41a,0x008,0x00b,0x806'),
 ("1/4 size (320x180; padded height 192)",
  [320],[180,192], RGBA8+',0x008,0x806'),
 ("1/8 size (160x90; padded 160x96, 16-bit formats pitch 192)",
  [160,192],[90,96], RGBA8+',0x008,0x806'),
 ("1/16 size (80x45 R16F; padded 128x48)",
  [80,128],[45,48], '0x806'),
]
L=[]
L.append("""[Definition]
titleIds = 0005000010102300
name = Wii Fit U - Resolution
path = "Wii Fit U/Graphics/Resolution"
description = Renders Wii Fit U above 1280x720. Scales the TV and GamePad images (the game renders both at 1280x720), their depth buffer and the 1/2, 1/4, 1/8 and 1/16 size bloom/blur buffers together. Mii icons, photos, Fit Meter icons, shadow maps and other textures stay native. Rules match by size and format, not by executable, so the pack works with retail and decomp builds (WFU_eq348 etc.).
version = 7
colorbufferOptimizationAware = true

# How it was made (fork notes: FORK_NOTES.md, "Wii Fit U resolution pack"): the sizes come from a static
# read of v81 Step.rpx (jet::draw::Texture::InitializeRenderTarget callers; jet ImageFormat -> GX2 table
# at 0x10099b50) and the padded sizes from Cemu's own LatteAddrLib (2D tiled thin1).
# - Only tiled surfaces scale (tileModesExcluded = linear): the game's CPU-read textures (photos/JPEG,
#   ShaderSupport screenshots) are linear (jet "type 3" textures), and Cemu can't read back a scaled texture.
# - Mii icons (RPSysMiiIcon/OliveMiiIcon/SAMUMiiIcon, CPU alpha pass after GX2Invalidate) are 256x256 or
#   smaller squares and match no rule. Neither do the 512x512 shadow map (R16G16F + D32F), the 256x256
#   R11G11B10F post-effect buffer, the 1x1..16x8 light-probe targets and the odd-sized minigame
#   targets (480x480 photos, 640x480, 1280x768, 740x740, 392x220...).
# - Compressed and 8/16-bit-per-channel CPU formats (BC1-5, R8, R8G8: textures and movie planes) are not
#   in any format list: a scaled texture that Cemu loads from memory comes out blank. A scan of every texture
#   in content/ (jtex, carc, jarc, jmdl; Yaz0 unpacked) found only one at a rule size and format: the 640x360
#   RGBA8 in Common/RPStepUnicycle/common.carc, which the rules leave out (1280x720 ones are all BC1/BC3).

[Default]
$scale = 2.0

[Preset]
name = 1280x720 (native)
$scale = 1.0

[Preset]
name = 1920x1080 (1.5x)
$scale = 1.5

[Preset]
name = 2560x1440 (2x)
$scale = 2.0
default = 1

[Preset]
name = 3840x2160 (3x)
$scale = 3.0
""")
for comment,ws,hs,fm in groups:
    L.append(f"# {comment}")
    for w in ws:
        for h in hs:
            L.append(f"""[TextureRedefine]
width = {w}
height = {h}
formats = {fm}
tileModesExcluded = 0x0,0x1
overwriteWidth = ($scale * {w})
overwriteHeight = ($scale * {h})
""")
open(out,'w').write('\n'.join(L))
