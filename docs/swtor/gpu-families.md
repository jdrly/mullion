# GPU families: what differs between M1 to M4 and the M5

What an Apple GPU family does differently that the fork answers with a switch. The switches are in
[README.md](README.md).

## Lossless texture compression

Metal compresses render targets and sampled textures losslessly on every Apple Silicon Mac, by default. Before Apple10
it doesn't when a texture's usage has `shaderWrite`, `pixelFormatView` or `unknown`, or its format is block-compressed;
on the M5 (Apple10) `shaderWrite` no longer turns it off. `allowGPUOptimizedContents = NO` turns it off on every
family, and is what the fork's switches set.

d9mt hit two bugs with it, both on an M4 (Apple9) and neither on an M5 Max. Nobody has seen either under Mullion yet:
the fork's rules come from d9mt's evidence.

| Bug | What answers it here |
|---|---|
| A compressed texture read before anything wrote it shows magenta (d9mt #101, #105) | `MULLION_TEX_NO_COMPRESSION=1`, for every texture. No default fix: Direct3D 12 calls a new texture's contents undefined, and clearing each one costs bandwidth on every Mac. Mullion doesn't clear new textures, and `DiscardResource` does nothing |
| 4x MSAA with compression draws flickering lines (d9mt #140; SWTOR's "Very High" anti-aliasing) | `MULLION_TEX_MSAA_COMPRESSION=auto` (or `0`): multisampled textures and their resolve targets without compression before Apple10, d9mt's fix, which the M4 player confirmed. `MULLION_TEX_NO_COMPRESSION=1` answers it too |

"8x" MSAA before Apple10 is made as paired 4x textures (`src/mullion/mullion_format.cpp`), so it meets the second
bug as 4x does.

What `MULLION_TEX_MSAA_COMPRESSION` can't see. Direct3D 12 doesn't say at creation which texture a resolve will
write, so a resolve target is a single-sampled 2D texture of the type group, width and height of a multisampled texture
the device has made before it. It misses a target made before its multisampled texture, a depth resolve into a color
format (`R32_FLOAT`), and a resolve into a larger texture; it also takes a full-size target of the same group that no
resolve writes, which then loses compression for nothing. d9mt resolves into an image of its own and copies, which a
Direct3D 12 command list can't do without a texture of its own per resolve. The `Mullion: texture uncompressed` lines
say which rules acted: an `msaa` line without a `resolve` line means this game's target was missed.

A texture placed in a heap gets its size from Metal with the same descriptor. Because a multisampled texture made
between `GetResourceAllocationInfo` and the placement can turn a texture into a resolve target, the first gives a
single-sampled 2D texture room for both descriptors while the rule is on.
