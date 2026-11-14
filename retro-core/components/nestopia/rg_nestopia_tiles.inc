// Exact execution of complete tiles with no intervening CPU access.
// Background bus accesses, cartridge callbacks and pixel/status updates
// retain their original dots. Private sprite shifters and idle evaluation work
// are combined between CPU accesses. Cartridge callbacks cannot inspect these
// private shifters or secondary-OAM evaluation state through the PPU API.

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
    *output.target++ = output.renderPalette[pixel];
}

NST_FORCE_INLINE void Ppu::RenderTilePixels(const word* sprites, uint clock)
{
    // Standard memory with no callback or the known IRQ-only callback cannot
    // observe intermediate screen stores or sprite-zero status. Bus operations
    // still execute at each original dot; compose this tile after those reads.
    word* NST_RESTRICT target = output.target;
    const word* NST_RESTRICT palette = output.renderPalette;
    const byte* NST_RESTRICT background = tiles.pixels;
    const uint mask = tiles.mask;
    const uint fine = scroll.xFine;
    // Most tiles contain no opaque sprite pixels. Keep their composition
    // branch-free rather than testing sprite priority and hit state per dot.
    if (!oam.mask || !(sprites[0] | sprites[1] | sprites[2] | sprites[3] |
                       sprites[4] | sprites[5] | sprites[6] | sprites[7]))
    {
        for (uint i=0; i<8; ++i)
            target[i] = palette[background[(clock + fine + i) & 15] & mask];
        output.target = target + 8;
        return;
    }
    uint hit = 0;
    for (uint i=0; i<8; ++i)
    {
        uint pixel = background[(clock + fine + i) & 15] & mask;
        const uint sprite = sprites[i];
        if (sprite)
        {
            if ((sprite & 0x80) && (pixel & 3)) hit = Regs::STATUS_SP_ZERO_HIT;
            if (!(sprite & 0x40) || !(pixel & 3)) pixel = sprite & 0x1F;
        }
        target[i] = palette[pixel];
    }
    regs.status |= hit;
    output.target = target + 8;
}

template<bool Direct>
RG_NES_PPU_HOT NST_NO_INLINE void Ppu::RenderTile()
{
    const uint dots = SpriteBatchDots(cycles.hClock, uint(cycles.count));
#ifdef RG_NESTOPIA_PROFILE
    RetroGo::tileBatches += dots / 8;
#endif
    word sprites[184];
    PrepareSpriteTile(sprites);
    // On verified standard-memory paths, neither CHR/nametable reads nor the
    // IRQ-only A12 observer can inspect secondary OAM. Complete its read/write
    // pairs together, before the unchanged bus fetches, up to this CPU boundary.
    // Keep arbitrary cartridge callbacks on the interleaved scalar phases.
    const bool combinedEvaluation = Direct && cycles.hClock >= 64;
    if (combinedEvaluation)
    {
        uint remaining = dots;
        if (oam.corrupt != Oam::NO_CORRUPTION)
        {
            // The first primary byte is latched before corruption copies the
            // row. Preserve that byte even when it belongs to the copied row.
            oam.latch = oam.ram[oam.address];
            CorruptOam();
            (*this.*oam.phase)();
            remaining -= 2;
        }
        while (remaining)
        {
            if (oam.phase == &Ppu::EvaluateSpritesPhase9)
            {
                ReadSecondaryOam();
                oam.address = (oam.address + 2*remaining) & 0xFF;
                break;
            }
            if (oam.phase == &Ppu::EvaluateSpritesPhase1)
            {
                // Misses overwrite the same secondary byte. Keep their
                // private counters local and commit the last write once.
                uint address=oam.address, index=oam.index, latch=0;
                bool hit=false;
                do
                {
                    latch=oam.ram[address];
                    ++index;
                    remaining-=2;
                    if (scanline-latch < oam.height)
                    {
                        oam.address=address; oam.index=index-1; oam.latch=latch;
                        EvaluateSpritesPhase1();
                        hit=true;
                        break;
                    }
                    if (index==64)
                    {
                        address=0;
                        oam.phase=&Ppu::EvaluateSpritesPhase9;
                        break;
                    }
                    address=(address+4)&0xFC;
                }
                while (remaining);
                if (!hit)
                {
                    oam.address=address; oam.index=index; oam.latch=latch;
                    if (oam.buffered!=oam.limit) oam.buffered[0]=latch;
                }
            }
            else if (oam.phase == &Ppu::EvaluateSpritesPhase2 && remaining>=6)
            {
                oam.latch=oam.ram[oam.address]; EvaluateSpritesPhase2();
                oam.latch=oam.ram[oam.address]; EvaluateSpritesPhase3();
                oam.latch=oam.ram[oam.address]; EvaluateSpritesPhase4();
                remaining-=6;
            }
            else
            {
                oam.latch = oam.ram[oam.address];
                (*this.*oam.phase)();
                remaining-=2;
            }
        }
    }
    const auto tile = [&](const word* sprite, bool evaluate) RG_NES_TILE_INNER_HOT {
        const uint clock = cycles.hClock;
        const auto pixel = [&](word color) {
            if (!Direct) RenderTilePixel(color);
        };
        // Select at compile time. The mapper path retains every original
        // fetch and address-line notification, including its dot timestamp.
        LoadTiles(); if (evaluate) { EvaluateSpritesEven(); }
        if (Direct) OpenNameTile(); else OpenName();
        pixel(sprite[0]);
        if (Direct) FetchNameTile(); else FetchName();
        if (evaluate) { EvaluateSpritesOdd(); } pixel(sprite[1]);
        if (Direct) cycles.hClock = clock + 2;
        if (evaluate) { EvaluateSpritesEven(); }
        if (Direct) OpenAttributeTile(); else OpenAttribute();
        pixel(sprite[2]);
        if (Direct) FetchAttributeTile(); else FetchAttribute();
        if (evaluate) { EvaluateSpritesOdd(); } pixel(sprite[3]);
        if (Direct) cycles.hClock = clock + 4;
        if (evaluate) { EvaluateSpritesEven(); }
        if (Direct) OpenPatternTile(io.pattern | 0x0); else OpenPattern(io.pattern | 0x0);
        pixel(sprite[4]);
        if (Direct) FetchBgPattern0Tile(); else FetchBgPattern0();
        if (evaluate) { EvaluateSpritesOdd(); } pixel(sprite[5]);
        if (Direct) cycles.hClock = clock + 6;
        if (evaluate) { EvaluateSpritesEven(); }
        if (Direct) OpenPatternTile(io.pattern | 0x8); else OpenPattern(io.pattern | 0x8);
        pixel(sprite[6]);
        if (Direct) FetchBgPattern1Tile(); else FetchBgPattern1();
        if (evaluate) { EvaluateSpritesOdd(); } scroll.ClockX(); pixel(sprite[7]);
        if (Direct) {
            cycles.hClock = clock + 8;
            RenderTilePixels(sprite, clock);
        }
        tiles.mask = tiles.show[0];
        oam.mask = oam.show[0];
    };
    uint offset = 0;
    if (combinedEvaluation)
    {
        for (; offset<dots; offset+=8)
            tile(sprites + offset, false);
    }
    else if (cycles.hClock < 64 && oam.phase == &Ppu::EvaluateSpritesPhase0)
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
