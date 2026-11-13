// Exact execution of complete tiles with no intervening CPU access or
// cartridge PPU callbacks. Background bus accesses and pixel/status updates
// retain their original dots. Private sprite shifters and idle evaluation work
// are combined between observable CPU/cartridge accesses.

static inline uint SpriteBatchDots(uint clock, uint count)
{
    // Return to the scheduler for the secondary-OAM reset at dot 64, and for
    // the final tile's special dot-255 sprite-zero handling.
    return (NST_MIN(count, clock < 64 ? 64U : 248U) & ~7U) - clock;
}

NST_FORCE_INLINE void Ppu::PrepareSpriteTile(word *pixels)
{
    const uint dots = SpriteBatchDots(cycles.hClock, uint(cycles.count));
    for (uint i=0; i<dots; ++i)
        pixels[i] = 0;

    for (Oam::Output *sprite=oam.output; sprite!=oam.visible; ++sprite)
    {
        if (sprite->shift == 8)
            continue;
        const idword wait = idword(sprite->deadline - oam.clock);
        if (wait >= idword(dots))
            continue;
        const uint first = wait > 0 ? uint(wait) : 0;
        const uint count = NST_MIN(dots-first, 8U-sprite->shift);
        const uint flags = (sprite->behind ? 0x40 : 0) | (sprite->zero ? 0x80 : 0);
        const byte *source = sprite->pixels + sprite->shift;
        for (uint i=0; i<count; ++i)
        {
            const uint color = source[i];
            if (color && !pixels[first+i])
                pixels[first+i] = sprite->palette + color + flags;
        }
        // Output shifters are private and are not read by background fetches or
        // OAM evaluation. Advance them once for this uninterrupted dot span.
        sprite->shift += count;
    }
}

NST_FORCE_INLINE void Ppu::RenderTilePixel(word sprite)
{
    uint pixel = tiles.pixels[(cycles.hClock++ + scroll.xFine) & 15] & tiles.mask;
    // The two supported masks are 0 (hidden/left-edge clipping) and 3.
    // Apply the current dot's mask after preparing the private shifters.
    if (!oam.mask)
        sprite = 0;
    if (sprite)
    {
        if ((sprite & 0x80) && (pixel & 3))
            regs.status |= Regs::STATUS_SP_ZERO_HIT;
        if (!(sprite & 0x40) || !(pixel & 3))
            pixel = sprite & 0x1F;
    }
    *output.target++ = output.palette[pixel];
}

RG_NES_PPU_HOT NST_NO_INLINE void Ppu::RenderTile()
{
    const uint dots = SpriteBatchDots(cycles.hClock, uint(cycles.count));
#ifdef RG_NESTOPIA_PROFILE
    RetroGo::tileBatches += dots / 8;
#endif
    word sprites[184];
    PrepareSpriteTile(sprites);
    const auto tile = [&](const word* sprite, bool evaluate) {
        LoadTiles(); if (evaluate) { EvaluateSpritesEven(); } OpenNameTile(); RenderTilePixel(sprite[0]);
        FetchNameTile(); if (evaluate) { EvaluateSpritesOdd(); } RenderTilePixel(sprite[1]);
        if (evaluate) { EvaluateSpritesEven(); } OpenAttributeTile(); RenderTilePixel(sprite[2]);
        FetchAttributeTile(); if (evaluate) { EvaluateSpritesOdd(); } RenderTilePixel(sprite[3]);
        if (evaluate) { EvaluateSpritesEven(); } OpenPatternTile(io.pattern | 0x0); RenderTilePixel(sprite[4]);
        FetchBgPattern0Tile(); if (evaluate) { EvaluateSpritesOdd(); } RenderTilePixel(sprite[5]);
        if (evaluate) { EvaluateSpritesEven(); } OpenPatternTile(io.pattern | 0x8); RenderTilePixel(sprite[6]);
        FetchBgPattern1Tile(); if (evaluate) { EvaluateSpritesOdd(); } scroll.ClockX(); RenderTilePixel(sprite[7]);
        tiles.mask = tiles.show[0];
        oam.mask = oam.show[0];
    };
    uint offset = 0;
    if (cycles.hClock < 64 && oam.phase == &Ppu::EvaluateSpritesPhase0)
    {
        // No CPU or cartridge callback can observe a partial clear in this
        // span, and phase 0 does not inspect the secondary-OAM bytes.
        if (oam.corrupt != Oam::NO_CORRUPTION)
            CorruptOam();
        std::memset(oam.buffer + (cycles.hClock >> 1), 0xFF, dots / 2);
        oam.latch = 0xFF;
        for (; offset<dots; offset+=8)
            tile(sprites + offset, false);
    }
    else
    {
        for (; offset<dots; offset+=8)
        {
            if (cycles.hClock >= 64 && oam.phase == &Ppu::EvaluateSpritesPhase9)
                break;
            tile(sprites + offset, true);
        }
        if (offset < dots)
        {
            // Completed evaluation only reads the same secondary-OAM byte
            // and advances the primary address by four on each odd dot.
            if (oam.corrupt != Oam::NO_CORRUPTION)
                CorruptOam();
            ReadSecondaryOam();
            oam.address = (oam.address + 2*(dots-offset)) & 0xFF;
            for (; offset<dots; offset+=8)
                tile(sprites + offset, false);
        }
    }
    oam.clock += dots;
}
