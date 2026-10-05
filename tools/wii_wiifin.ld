/*------------------------------------------------------------------------------
** wii_wiifin.ld — Wii linker script for WiiFin
**
** Memory layout (low → high):
**   text    0x80003f00   code
**   data    0x8043xxxx   rodata, data, sdata
**   sbss    right after sdata (NOLOAD)
**   bss     right after sbss  (NOLOAD)  ← contiguous with sbss
**   blobs   after bss, ALIGN(32)        ← DOL text[1], safe from BSS zeroing
**
** BSS (sbss+bss) is ONE contiguous PHDR so the DOL BSS region never
** overlaps with the blob text segment.  Previously blobs sat between
** sbss and bss, so the DOL loader zeroed them after loading → crash.
**------------------------------------------------------------------------------*/

OUTPUT_ARCH(powerpc:common)
ENTRY(__wiifin_start)

PROVIDE_HIDDEN( __isIPL = 0 );

PROVIDE_HIDDEN( __ppc_excpt_buf   = 0x800000d0 );
PROVIDE_HIDDEN( __ppc_excpt_table = 0x80003000 );

PHDRS {
        stub  PT_LOAD FLAGS(5);
        text  PT_LOAD FLAGS(5);
        data  PT_LOAD FLAGS(6);
        bss   PT_LOAD;
        text2 PT_LOAD FLAGS(5);        /* blobs as second "text" → DOL text[1] */
}

SECTIONS {

        /* Optional stub section (CustomizeMii forwarder / crt0 stub) */
        . = 0x00003400;
        .stub : {
                KEEP(*(.stub))
        } :stub = 0

        /* ------------------------------------------------------------------ */
        /* TEXT                                                                */
        /* ------------------------------------------------------------------ */
        . = 0x80003f00;

        .init : {
                *(.crt0_pre .crt0_pre.*)
                *(.crt0 .crt0.*)
        } :text = 0

        .text : ALIGN_WITH_INPUT {
                KEEP( *(SORT_NONE(.init)) )
                *(.text.unlikely .text.*_unlikely .text.unlikely.*)
                *(.text.exit .text.exit.*)
                *(.text.startup .text.startup.*)
                *(.text.hot .text.hot.*)
                *(SORT(.text.sorted.*))
                *(.text .iplt .stub .text.* .gnu.linkonce.t.*)
                KEEP( *(SORT_NONE(.fini)) )
                . = ALIGN(32);   /* DOL loaders round section sizes up to 32 bytes */
        } :text = 0

        . = ALIGN(32);

        /* ------------------------------------------------------------------ */
        /* DATA (normal — must stay below 0x80804000)                         */
        /* Large blobs are excluded; they go into .data.blobs (text2) below. */
        /* ------------------------------------------------------------------ */

        .rodata : {
                KEEP( *(.rodata.__sys_versioninfo) )
                *(.rodata .rodata.* .gnu.linkonce.r.*)
                *(.rodata1)
                *(.fixup)
                *(.got1)
                *(.got2)
                *(.branch_lt)
                *(.got)
                *(.plt)
        } :data = 0

        .sdata2 : ALIGN_WITH_INPUT {
                *(.sdata2 .sdata2.* .gnu.linkonce.s2.*)
                *(.sbss2 .sbss2.* .gnu.linkonce.sb2.*)
        } :data = 0

        .eh_frame_hdr : ALIGN_WITH_INPUT {
                *(.eh_frame_hdr)
                *(.eh_frame_entry .eh_frame_entry.*)
        } :data = 0

        .eh_frame : ALIGN_WITH_INPUT {
                KEEP( *(.eh_frame) )
                *(.eh_frame.*)
        } :data = 0

        .gcc_except_table : ALIGN_WITH_INPUT {
                *(.gcc_except_table .gcc_except_table.*)
        } :data = 0

        .gnu_extab : ALIGN_WITH_INPUT {
                *(.gnu_extab*)
        } :data = 0

        .ctors : ALIGN_WITH_INPUT {
                KEEP( *ecrti.o(.ctors) )
                KEEP( *crtbegin.o(.ctors) )
                KEEP( *crtbegin?.o(.ctors) )
                KEEP( *(EXCLUDE_FILE(*ecrtn.o *crtend.o *crtend?.o) .ctors) )
                KEEP( *(SORT_BY_NAME(.ctors.*)) )
                KEEP( *(SORT_NONE(.ctors)) )
        } :data = 0

        .dtors : ALIGN_WITH_INPUT {
                KEEP( *ecrti.o(.dtors) )
                KEEP( *crtbegin.o(.dtors) )
                KEEP( *crtbegin?.o(.dtors) )
                KEEP( *(EXCLUDE_FILE(*ecrtn.o *crtend.o *crtend?.o) .dtors) )
                KEEP( *(SORT_BY_NAME(.dtors.*)) )
                KEEP( *(SORT_NONE(.dtors)) )
        } :data = 0

        /* Normal mutable data — exclude large blobs */
        .data : ALIGN_WITH_INPUT {
                EXCLUDE_FILE(*wii_font_ttf.o *jp_font_ttf.o *cacert_pem.o *music_mp3.o) *(.data .data.* .gnu.linkonce.d.*)
                *(.data1)
                SORT(CONSTRUCTORS)
        } :data = 0

        .sdata : ALIGN_WITH_INPUT {
                *(.sdata .sdata.* .gnu.linkonce.s.*)
                *(.lit8)
                *(.lit4)
                . = ALIGN(32);   /* DOL loaders round section sizes up to 32 bytes */
        } :data = 0

        /* ------------------------------------------------------------------ */
        /* sbss + bss — contiguous, right after sdata.                      */
        /* crt0 zeros __sbss_start → __bss_end covering both sections.      */
        /* ------------------------------------------------------------------ */
        .sbss . (NOLOAD) : {
                *(.dynsbss)
                *(.sbss .sbss.* .gnu.linkonce.sb.*)
                *(.scommon)
        } :bss

        /* NAND-loader stub zone (0x80804000-0x80831E40): App copies the stub
         * back there right before returning to the Wii Menu, overwriting it.
         * Only buffers that are dead by then (DEAD_AT_EXIT, see
         * source/core/ExitZone.h) may sit in it; all other BSS starts past
         * its end.  If the dead buffers don't reach that far, the rest of the
         * zone is simply left unused. */
        .bss.dead (NOLOAD) : {
                *(.bss.dead_at_exit)
        } :bss

        ASSERT(ADDR(.bss.dead) <= 0x80804000,
               "data/sbss grew into the NAND-loader stub zone at 0x80804000")

        . = MAX(., 0x80831E40);

        .bss (NOLOAD) : {
                *(.dynbss)
                *(.bss .bss.* .gnu.linkonce.b.*)
                *(COMMON)
        } :bss

        PROVIDE_HIDDEN( __sbss_start = ADDR(.sbss)                );
        PROVIDE_HIDDEN( __sbss_end   = ADDR(.sbss) + SIZEOF(.sbss) );
        PROVIDE_HIDDEN( __bss_start  = ADDR(.bss)                 );
        PROVIDE_HIDDEN( __bss_end    = ADDR(.bss) + SIZEOF(.bss)  );

        /* ------------------------------------------------------------------ */
        /* Large blobs — AFTER bss so the DOL BSS region never overlaps.    */
        /* FLAGS(5) → elf2dol puts them in DOL text[1].                     */
        /* ------------------------------------------------------------------ */
        .data.blobs ALIGN(32) : {
                *wii_font_ttf.o(.data .data.*)
                *jp_font_ttf.o(.data .data.*)
                *cacert_pem.o(.data .data.*)
                *music_mp3.o(.data .data.*)
                *stub_zone.o(.stub_zone)
                . = ALIGN(32);   /* DOL loaders round section sizes up to 32 bytes */
        } :text2 = 0

        . = ADDR(.data.blobs) + SIZEOF(.data.blobs);

        . = .;

        PROVIDE_HIDDEN( __Arena1Lo = (. + 0x1f) &~ 0x1f );
        PROVIDE_HIDDEN( __Arena1Hi = 0x81800000         );

        PROVIDE_HIDDEN( __Arena2Lo    = 0x90002000 );
        PROVIDE_HIDDEN( __Arena2Hi    = 0x933e0000 );
        PROVIDE_HIDDEN( __ipcbufferLo = 0x933e0000 );
        PROVIDE_HIDDEN( __ipcbufferHi = 0x93400000 );

        /* Debugging metadata (discarded from final binary) */
        .stab           0 : { *(.stab) }
        .stabstr        0 : { *(.stabstr) }
        .comment 0 (INFO) : { *(.comment); LINKER_VERSION; }
        .gnu.build.attributes : { *(.gnu.build.attributes .gnu.build.attributes.*) }
        .debug          0 : { *(.debug) }
        .line           0 : { *(.line) }
        .debug_aranges  0 : { *(.debug_aranges) }
        .debug_pubnames 0 : { *(.debug_pubnames) }
        .debug_info     0 : { *(.debug_info .gnu.linkonce.wi.*) }
        .debug_abbrev   0 : { *(.debug_abbrev) }
        .debug_line     0 : { *(.debug_line .debug_line.* .debug_line_end) }
        .debug_frame    0 : { *(.debug_frame) }
        .debug_str      0 : { *(.debug_str) }
        .debug_loc      0 : { *(.debug_loc) }
        .debug_macinfo  0 : { *(.debug_macinfo) }
        .debug_macro    0 : { *(.debug_macro) }
        .debug_ranges   0 : { *(.debug_ranges) }
        .debug_pubtypes 0 : { *(.debug_pubtypes) }

        /DISCARD/ : { *(.note.GNU-stack) *(.gnu_debuglink) *(.gnu.lto_*) }
}
