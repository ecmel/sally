// The built-in OS, for running without the Atari OS ROM. It does what
// cartridge games ask of the OS: reset, the NMI and IRQ dispatch through
// the RAM vectors, the vertical blank's two stages with their shadow
// registers and timers, SETVBV, and a character set at $E000 (drawn from
// scratch, not Atari's). Entry points and vectors are at the addresses of
// the XL OS. There is no CIO or SIO: those calls return an error.
//
// It is assembled at startup from the source below.

#include "asm6502.h"
#include "machine.h"

#include <string.h>

static const char *const source =
    "; Page zero and OS variables\n"
    "POKMSK = $10\n"
    "BRKKEY = $11\n"
    "RTCLOK = $12\n"
    "CRITIC = $42\n"
    "ATRACT = $4D\n"
    "DRKMSK = $4E\n"
    "COLRSH = $4F\n"
    "RAMTOP = $6A\n"
    "VDSLST = $0200\n"
    "VTIMR1 = $0210\n"
    "VTIMR2 = $0212\n"
    "VTIMR4 = $0214\n"
    "VIMIRQ = $0216\n"
    "VKEYBD = $0208\n"
    "VSERIN = $020A\n"
    "VSEROR = $020C\n"
    "VSEROC = $020E\n"
    "CDTMV1 = $0218\n"
    "CDTMV2 = $021A\n"
    "CDTMV3 = $021C\n"
    "VVBLKI = $0222\n"
    "VVBLKD = $0224\n"
    "CDTMA1 = $0226\n"
    "CDTMA2 = $0228\n"
    "CDTMF3 = $022A\n"
    "INTEMP = $022D\n"
    "SDMCTL = $022F\n"
    "SDLSTL = $0230\n"
    "SDLSTH = $0231\n"
    "GPRIOR = $026F\n"
    "STICK0 = $0278\n"
    "STICK1 = $0279\n"
    "STRIG0 = $0284\n"
    "STRIG1 = $0285\n"
    "PCOLR0 = $02C0\n"
    "COLOR0 = $02C4\n"
    "CHACT  = $02F3\n"
    "CHBAS  = $02F4\n"
    "CH     = $02FC\n"
    "; Hardware\n"
    "TRIG0  = $D010\n"
    "TRIG1  = $D011\n"
    "COLPM0 = $D012\n"
    "PRIOR  = $D01B\n"
    "CONSOL = $D01F\n"
    "KBCODE = $D209\n"
    "IRQEN  = $D20E\n"
    "IRQST  = $D20E\n"
    "SKCTL  = $D20F\n"
    "PORTA  = $D300\n"
    "PORTB  = $D301\n"
    "PACTL  = $D302\n"
    "PBCTL  = $D303\n"
    "DMACTL = $D400\n"
    "CHACTL = $D401\n"
    "DLISTL = $D402\n"
    "DLISTH = $D403\n"
    "CHBASE = $D409\n"
    "NMIEN  = $D40E\n"
    "NMIST  = $D40F\n"
    "NMIRES = $D40F\n"
    "CART   = $BFFA\n"
    "CARTCK = $BFFC\n"
    "CARTFG = $BFFD\n"
    "CARTIN = $BFFE\n"
    "\n"
    "        .org $E450\n"
    "        jmp nodev       ; DISKIV\n"
    "        jmp nodev       ; DSKINV\n"
    "        jmp nodev       ; CIOV\n"
    "        jmp nodev       ; SIOV\n"
    "        jmp setvbv      ; SETVBV $E45C\n"
    "        jmp sysvbv      ; SYSVBV $E45F\n"
    "        jmp xitvbv      ; XITVBV $E462\n"
    "        jmp return      ; SIOINV\n"
    "        jmp return      ; SENDEV\n"
    "        jmp return      ; INTINV\n"
    "        jmp return      ; CIOINV\n"
    "        jmp reset       ; BLKBDV\n"
    "        jmp reset       ; WARMSV\n"
    "        jmp reset       ; COLDSV\n"
    "\n"
    "nodev:  ldy #$82        ; nonexistent device\n"
    "return: rts\n"
    "\n"
    "; Z is set when a cartridge is present: $BFFC is zero and is ROM.\n"
    "hascart: lda CARTCK\n"
    "        bne cknone\n"
    "        inc CARTCK\n"
    "        lda CARTCK\n"
    "        bne ckram\n"
    "        rts\n"
    "ckram:  dec CARTCK\n"
    "cknone: lda #1\n"
    "        rts\n"
    "\n"
    "reset:  sei\n"
    "        cld\n"
    "        ldx #$FF\n"
    "        txs\n"
    "        ; A diagnostic cartridge takes over before anything is touched.\n"
    "        jsr hascart\n"
    "        bne cold\n"
    "        lda CARTFG\n"
    "        bpl cold\n"
    "        jmp (CARTIN)\n"
    "\n"
    "cold:   lda #0\n"
    "        tax\n"
    "clrhw:  sta $D000,x\n"
    "        sta $D200,x\n"
    "        sta $D400,x\n"
    "        inx\n"
    "        bne clrhw\n"
    "        ; PORTB: OS ROM on, BASIC off. The latch is set before the\n"
    "        ; lines become outputs, or the ROM would vanish under us.\n"
    "        lda #$3C\n"
    "        sta PBCTL\n"
    "        sta PACTL\n"
    "        lda #$FF\n"
    "        sta PORTB\n"
    "        lda #$38\n"
    "        sta PBCTL\n"
    "        lda #$FF\n"
    "        sta PORTB\n"
    "        lda #$3C\n"
    "        sta PBCTL\n"
    "        lda #3\n"
    "        sta SKCTL\n"
    "        ; Clear the OS's RAM: zero page and pages 2 and 3. The rest is\n"
    "        ; clear at power on (Sally starts with zeroed RAM) and kept over\n"
    "        ; RESET, as on the XL.\n"
    "        lda #0\n"
    "        tax\n"
    "clrram: sta $00,x\n"
    "        sta $0200,x\n"
    "        sta $0300,x\n"
    "        inx\n"
    "        bne clrram\n"
    "        ; Vectors and timers.\n"
    "        ldx #vecend-vectab-1\n"
    "vecl:   lda vectab,x\n"
    "        sta VDSLST,x\n"
    "        dex\n"
    "        bpl vecl\n"
    "        ; The screen: blank, black, until a cartridge takes over.\n"
    "        lda #<dlist\n"
    "        sta SDLSTL\n"
    "        lda #>dlist\n"
    "        sta SDLSTH\n"
    "        lda #$22\n"
    "        sta SDMCTL\n"
    "        lda #$E0\n"
    "        sta CHBAS\n"
    "        lda #2\n"
    "        sta CHACT\n"
    "        lda #$C0\n"
    "        sta RAMTOP\n"
    "        lda #$FF\n"
    "        sta CH\n"
    "        lda #$C0        ; keyboard and BREAK interrupts\n"
    "        sta POKMSK\n"
    "        sta IRQEN\n"
    "        lda #8\n"
    "        sta CONSOL\n"
    "        lda #$40        ; vertical blank interrupt\n"
    "        sta NMIEN\n"
    "        cli\n"
    "        jsr hascart\n"
    "        bne idle\n"
    "        lda #$A0\n"
    "        sta RAMTOP\n"
    "        ; The colors the Atari OS leaves; games rely on them.\n"
    "        ldx #4\n"
    "coll:   lda coltab,x\n"
    "        sta COLOR0,x\n"
    "        dex\n"
    "        bpl coll\n"
    "        jsr initc\n"
    "        lda CARTFG\n"
    "        and #4\n"
    "        beq idle\n"
    "        jmp (CART)\n"
    "initc:  jmp (CARTIN)\n"
    "idle:   jmp idle\n"
    "\n"
    "vectab: .word rtiv      ; VDSLST\n"
    "        .word plarti    ; VPRCED\n"
    "        .word plarti    ; VINTER\n"
    "        .word plarti    ; VBREAK\n"
    "        .word keyirq    ; VKEYBD\n"
    "        .word plarti    ; VSERIN\n"
    "        .word plarti    ; VSEROR\n"
    "        .word plarti    ; VSEROC\n"
    "        .word plarti    ; VTIMR1\n"
    "        .word plarti    ; VTIMR2\n"
    "        .word plarti    ; VTIMR4\n"
    "        .word irqdef    ; VIMIRQ\n"
    "        .word 0,0,0,0,0 ; CDTMV1-5\n"
    "        .word sysvbv    ; VVBLKI\n"
    "        .word xitvbv    ; VVBLKD\n"
    "vecend:\n"
    "coltab: .byte $28,$CA,$94,$46,$00 ; COLOR0-4\n"
    "\n"
    "nmi:    bit NMIST\n"
    "        bpl notdli\n"
    "        jmp (VDSLST)\n"
    "notdli: cld\n"
    "        pha\n"
    "        txa\n"
    "        pha\n"
    "        tya\n"
    "        pha\n"
    "        sta NMIRES\n"
    "        jmp (VVBLKI)\n"
    "\n"
    "; Stage 1: the clock, attract mode and timer 1.\n"
    "sysvbv: inc RTCLOK+2\n"
    "        bne vb1\n"
    "        inc ATRACT\n"
    "        inc RTCLOK+1\n"
    "        bne vb1\n"
    "        inc RTCLOK\n"
    "vb1:    lda #$FE\n"
    "        ldx #0\n"
    "        ldy ATRACT\n"
    "        bpl vb2\n"
    "        sta ATRACT\n"
    "        ldx RTCLOK+1\n"
    "        lda #$F6\n"
    "vb2:    sta DRKMSK\n"
    "        stx COLRSH\n"
    "        lda CDTMV1\n"
    "        bne t1lo\n"
    "        lda CDTMV1+1\n"
    "        beq vb3\n"
    "        dec CDTMV1+1\n"
    "t1lo:   dec CDTMV1\n"
    "        lda CDTMV1\n"
    "        ora CDTMV1+1\n"
    "        bne vb3\n"
    "        jsr cdt1\n"
    "        ; Stage 2 only outside critical sections.\n"
    "vb3:    lda CRITIC\n"
    "        bne vbexit\n"
    "        tsx\n"
    "        lda $0104,x     ; P as the interrupt found it\n"
    "        and #$04\n"
    "        beq stage2\n"
    "vbexit: jmp (VVBLKD)\n"
    "stage2: lda SDLSTL\n"
    "        sta DLISTL\n"
    "        lda SDLSTH\n"
    "        sta DLISTH\n"
    "        lda SDMCTL\n"
    "        sta DMACTL\n"
    "        lda GPRIOR\n"
    "        sta PRIOR\n"
    "        ldx #8\n"
    "vbcol:  lda PCOLR0,x\n"
    "        eor COLRSH\n"
    "        and DRKMSK\n"
    "        sta COLPM0,x\n"
    "        dex\n"
    "        bpl vbcol\n"
    "        lda CHBAS\n"
    "        sta CHBASE\n"
    "        lda CHACT\n"
    "        sta CHACTL\n"
    "        lda #8\n"
    "        sta CONSOL\n"
    "        lda CDTMV2\n"
    "        bne t2lo\n"
    "        lda CDTMV2+1\n"
    "        beq vb4\n"
    "        dec CDTMV2+1\n"
    "t2lo:   dec CDTMV2\n"
    "        lda CDTMV2\n"
    "        ora CDTMV2+1\n"
    "        bne vb4\n"
    "        jsr cdt2\n"
    "        ; Timers 3-5 clear their flags when they run out.\n"
    "vb4:    ldx #4\n"
    "tml:    lda CDTMV3,x\n"
    "        ora CDTMV3+1,x\n"
    "        beq tmn\n"
    "        lda CDTMV3,x\n"
    "        bne tmlo\n"
    "        dec CDTMV3+1,x\n"
    "tmlo:   dec CDTMV3,x\n"
    "        lda CDTMV3,x\n"
    "        ora CDTMV3+1,x\n"
    "        bne tmn\n"
    "        sta CDTMF3,x\n"
    "tmn:    dex\n"
    "        dex\n"
    "        bpl tml\n"
    "        lda PORTA\n"
    "        tay\n"
    "        and #$0F\n"
    "        sta STICK0\n"
    "        tya\n"
    "        lsr a\n"
    "        lsr a\n"
    "        lsr a\n"
    "        lsr a\n"
    "        sta STICK1\n"
    "        lda TRIG0\n"
    "        sta STRIG0\n"
    "        lda TRIG1\n"
    "        sta STRIG1\n"
    "exitvb: jmp (VVBLKD)\n"
    "xitvbv: pla\n"
    "        tay\n"
    "        pla\n"
    "        tax\n"
    "        pla\n"
    "rtiv:   rti\n"
    "cdt1:   jmp (CDTMA1)\n"
    "cdt2:   jmp (CDTMA2)\n"
    "\n"
    "; SETVBV: A = timer 1-5, 6 for VVBLKI, 7 for VVBLKD; X high, Y low.\n"
    "setvbv: asl a\n"
    "        sta INTEMP\n"
    "        txa\n"
    "        ldx INTEMP\n"
    "        sta CDTMV1-1,x\n"
    "        tya\n"
    "        sta CDTMV1-2,x\n"
    "        rts\n"
    "\n"
    "irq:    jmp (VIMIRQ)\n"
    "; Finds the interrupt's source, acknowledges it and goes through its vector.\n"
    "irqdef: pha\n"
    "        lda IRQST\n"
    "        and #$40\n"
    "        bne irq2\n"
    "        lda #$BF\n"
    "        sta IRQEN\n"
    "        lda POKMSK\n"
    "        sta IRQEN\n"
    "        jmp (VKEYBD)\n"
    "irq2:   lda IRQST\n"
    "        bmi irq3\n"
    "        lda #$7F\n"
    "        sta IRQEN\n"
    "        lda POKMSK\n"
    "        sta IRQEN\n"
    "        lda #0\n"
    "        sta BRKKEY\n"
    "plarti: pla\n"
    "        rti\n"
    "irq3:   lda IRQST\n"
    "        lsr a\n"
    "        bcs irq4\n"
    "        lda #$FE\n"
    "        jsr ack\n"
    "        jmp (VTIMR1)\n"
    "irq4:   lsr a\n"
    "        bcs irq5\n"
    "        lda #$FD\n"
    "        jsr ack\n"
    "        jmp (VTIMR2)\n"
    "irq5:   lsr a\n"
    "        bcs irq6\n"
    "        lda #$FB\n"
    "        jsr ack\n"
    "        jmp (VTIMR4)\n"
    "irq6:   lsr a\n"
    "        bcs irq7\n"
    "        lda #$F7\n"
    "        jsr ack\n"
    "        jmp (VSEROC)\n"
    "irq7:   lsr a\n"
    "        bcs irq8\n"
    "        lda #$EF\n"
    "        jsr ack\n"
    "        jmp (VSEROR)\n"
    "irq8:   lsr a\n"
    "        bcs plarti\n"
    "        lda #$DF\n"
    "        jsr ack\n"
    "        jmp (VSERIN)\n"
    "ack:    and POKMSK\n"
    "        sta IRQEN\n"
    "        lda POKMSK\n"
    "        sta IRQEN\n"
    "        rts\n"
    "\n"
    "keyirq: lda KBCODE\n"
    "        sta CH\n"
    "        lda #0\n"
    "        sta ATRACT\n"
    "        pla\n"
    "        rti\n"
    "\n"
    "; The display list may not cross a 1K boundary.\n"
    "        .org $F000\n"
    "dlist:  .byte $41\n"
    "        .word dlist\n"
    "\n"
    "        .org $FFFA\n"
    "        .word nmi, reset, irq\n";

// A stand-in for the OS character set, by internal character code.
static const struct {
    uint8_t code;
    const char *rows[8];
} glyphs[] = {
    {0x01, {"........", "...##...", "...##...", "...##...", "...##...", "........", "...##...", "........"}}, // !
    {0x02, {"........", ".##..##.", ".##..##.", ".##..##.", "........", "........", "........", "........"}}, // "
    {0x03, {"........", ".##..##.", "########", ".##..##.", ".##..##.", "########", ".##..##.", "........"}}, // #
    {0x04, {"...##...", "..#####.", ".##.....", "..####..", ".....##.", ".#####..", "...##...", "........"}}, // $
    {0x05, {"........", ".##...##", ".##..##.", "....##..", "...##...", "..##..##", ".##...##", "........"}}, // %
    {0x06, {"........", "..###...", ".##.##..", "..###...", ".###.###", ".##.##..", "..###.##", "........"}}, // &
    {0x07, {"........", "...##...", "...##...", "...##...", "........", "........", "........", "........"}}, // '
    {0x08, {"........", "....###.", "...###..", "...##...", "...##...", "...###..", "....###.", "........"}}, // (
    {0x09, {"........", ".###....", "..###...", "...##...", "...##...", "..###...", ".###....", "........"}}, // )
    {0x0A, {"........", ".##..##.", "..####..", "########", "..####..", ".##..##.", "........", "........"}}, // *
    {0x0B, {"........", "...##...", "...##...", ".######.", "...##...", "...##...", "........", "........"}}, // +
    {0x0C, {"........", "........", "........", "........", "........", "...##...", "...##...", "..##...."}}, // ,
    {0x0D, {"........", "........", "........", ".######.", "........", "........", "........", "........"}}, // -
    {0x0E, {"........", "........", "........", "........", "........", "...##...", "...##...", "........"}}, // .
    {0x0F, {"........", ".....##.", "....##..", "...##...", "..##....", ".##.....", ".#......", "........"}}, // /
    {0x10, {"........", "..####..", ".##..##.", ".##.###.", ".###.##.", ".##..##.", "..####..", "........"}}, // 0
    {0x11, {"........", "...##...", "..###...", "...##...", "...##...", "...##...", ".######.", "........"}}, // 1
    {0x12, {"........", "..####..", ".##..##.", "....##..", "...##...", "..##....", ".######.", "........"}}, // 2
    {0x13, {"........", ".######.", "....##..", "...##...", "....##..", ".##..##.", "..####..", "........"}}, // 3
    {0x14, {"........", "....##..", "...###..", "..####..", ".##.##..", ".######.", "....##..", "........"}}, // 4
    {0x15, {"........", ".######.", ".##.....", ".#####..", ".....##.", ".##..##.", "..####..", "........"}}, // 5
    {0x16, {"........", "..####..", ".##.....", ".#####..", ".##..##.", ".##..##.", "..####..", "........"}}, // 6
    {0x17, {"........", ".######.", ".....##.", "....##..", "...##...", "..##....", "..##....", "........"}}, // 7
    {0x18, {"........", "..####..", ".##..##.", "..####..", ".##..##.", ".##..##.", "..####..", "........"}}, // 8
    {0x19, {"........", "..####..", ".##..##.", "..#####.", ".....##.", "....##..", "..###...", "........"}}, // 9
    {0x1A, {"........", "........", "...##...", "...##...", "........", "...##...", "...##...", "........"}}, // :
    {0x1B, {"........", "........", "...##...", "...##...", "........", "...##...", "...##...", "..##...."}}, // ;
    {0x1C, {"........", "....##..", "...##...", "..##....", "...##...", "....##..", "........", "........"}}, // <
    {0x1D, {"........", "........", ".######.", "........", ".######.", "........", "........", "........"}}, // =
    {0x1E, {"........", ".##.....", "..##....", "...##...", "..##....", ".##.....", "........", "........"}}, // >
    {0x1F, {"........", "..####..", ".##..##.", "....##..", "...##...", "........", "...##...", "........"}}, // ?
    {0x20, {"........", "..####..", ".##..##.", ".##.###.", ".##.###.", ".##.....", "..####..", "........"}}, // @
    {0x21, {"........", "...##...", "..####..", ".##..##.", ".##..##.", ".######.", ".##..##.", "........"}}, // A
    {0x22, {"........", ".#####..", ".##..##.", ".#####..", ".##..##.", ".##..##.", ".#####..", "........"}}, // B
    {0x23, {"........", "..####..", ".##..##.", ".##.....", ".##.....", ".##..##.", "..####..", "........"}}, // C
    {0x24, {"........", ".####...", ".##.##..", ".##..##.", ".##..##.", ".##.##..", ".####...", "........"}}, // D
    {0x25, {"........", ".######.", ".##.....", ".#####..", ".##.....", ".##.....", ".######.", "........"}}, // E
    {0x26, {"........", ".######.", ".##.....", ".#####..", ".##.....", ".##.....", ".##.....", "........"}}, // F
    {0x27, {"........", "..#####.", ".##.....", ".##.....", ".##.###.", ".##..##.", "..#####.", "........"}}, // G
    {0x28, {"........", ".##..##.", ".##..##.", ".######.", ".##..##.", ".##..##.", ".##..##.", "........"}}, // H
    {0x29, {"........", ".######.", "...##...", "...##...", "...##...", "...##...", ".######.", "........"}}, // I
    {0x2A, {"........", ".....##.", ".....##.", ".....##.", ".....##.", ".##..##.", "..####..", "........"}}, // J
    {0x2B, {"........", ".##..##.", ".##.##..", ".####...", ".####...", ".##.##..", ".##..##.", "........"}}, // K
    {0x2C, {"........", ".##.....", ".##.....", ".##.....", ".##.....", ".##.....", ".######.", "........"}}, // L
    {0x2D, {"........", ".##...##", ".###.###", ".#######", ".##.#.##", ".##...##", ".##...##", "........"}}, // M
    {0x2E, {"........", ".##..##.", ".###.##.", ".######.", ".######.", ".##.###.", ".##..##.", "........"}}, // N
    {0x2F, {"........", "..####..", ".##..##.", ".##..##.", ".##..##.", ".##..##.", "..####..", "........"}}, // O
    {0x30, {"........", ".#####..", ".##..##.", ".##..##.", ".#####..", ".##.....", ".##.....", "........"}}, // P
    {0x31, {"........", "..####..", ".##..##.", ".##..##.", ".##..##.", ".##.##..", "..##.##.", "........"}}, // Q
    {0x32, {"........", ".#####..", ".##..##.", ".##..##.", ".#####..", ".##.##..", ".##..##.", "........"}}, // R
    {0x33, {"........", "..####..", ".##.....", "..####..", ".....##.", ".....##.", ".#####..", "........"}}, // S
    {0x34, {"........", ".######.", "...##...", "...##...", "...##...", "...##...", "...##...", "........"}}, // T
    {0x35, {"........", ".##..##.", ".##..##.", ".##..##.", ".##..##.", ".##..##.", ".######.", "........"}}, // U
    {0x36, {"........", ".##..##.", ".##..##.", ".##..##.", ".##..##.", "..####..", "...##...", "........"}}, // V
    {0x37, {"........", ".##...##", ".##...##", ".##.#.##", ".#######", ".###.###", ".##...##", "........"}}, // W
    {0x38, {"........", ".##..##.", ".##..##.", "..####..", "..####..", ".##..##.", ".##..##.", "........"}}, // X
    {0x39, {"........", ".##..##.", ".##..##.", "..####..", "...##...", "...##...", "...##...", "........"}}, // Y
    {0x3A, {"........", ".######.", "....##..", "...##...", "..##....", ".##.....", ".######.", "........"}}, // Z
    {0x3B, {"........", "..####..", "..##....", "..##....", "..##....", "..##....", "..####..", "........"}}, // [
    {0x3C, {"........", ".#......", ".##.....", "..##....", "...##...", "....##..", ".....##.", "........"}}, // backslash
    {0x3D, {"........", "..####..", "....##..", "....##..", "....##..", "....##..", "..####..", "........"}}, // ]
    {0x3E, {"........", "...#....", "..###...", ".##.##..", "##...##.", "........", "........", "........"}}, // ^
    {0x3F, {"........", "........", "........", "........", "........", "........", "########", "........"}}, // _
    {0x60, {"........", "...##...", "..####..", ".######.", ".######.", "..####..", "...##...", "........"}}, // diamond
    {0x61, {"........", "........", "..####..", ".....##.", "..#####.", ".##..##.", "..#####.", "........"}}, // a
    {0x62, {"........", ".##.....", ".##.....", ".#####..", ".##..##.", ".##..##.", ".#####..", "........"}}, // b
    {0x63, {"........", "........", "..####..", ".##.....", ".##.....", ".##.....", "..####..", "........"}}, // c
    {0x64, {"........", ".....##.", ".....##.", "..#####.", ".##..##.", ".##..##.", "..#####.", "........"}}, // d
    {0x65, {"........", "........", "..####..", ".##..##.", ".######.", ".##.....", "..####..", "........"}}, // e
    {0x66, {"........", "...###..", "..##....", ".#####..", "..##....", "..##....", "..##....", "........"}}, // f
    {0x67, {"........", "........", "..#####.", ".##..##.", ".##..##.", "..#####.", ".....##.", ".#####.."}}, // g
    {0x68, {"........", ".##.....", ".##.....", ".#####..", ".##..##.", ".##..##.", ".##..##.", "........"}}, // h
    {0x69, {"........", "...##...", "........", "..###...", "...##...", "...##...", "..####..", "........"}}, // i
    {0x6A, {"........", ".....##.", "........", ".....##.", ".....##.", ".....##.", ".##..##.", "..####.."}}, // j
    {0x6B, {"........", ".##.....", ".##.....", ".##.##..", ".####...", ".##.##..", ".##..##.", "........"}}, // k
    {0x6C, {"........", "..###...", "...##...", "...##...", "...##...", "...##...", "..####..", "........"}}, // l
    {0x6D, {"........", "........", ".##..##.", ".#######", ".#######", ".##.#.##", ".##...##", "........"}}, // m
    {0x6E, {"........", "........", ".#####..", ".##..##.", ".##..##.", ".##..##.", ".##..##.", "........"}}, // n
    {0x6F, {"........", "........", "..####..", ".##..##.", ".##..##.", ".##..##.", "..####..", "........"}}, // o
    {0x70, {"........", "........", ".#####..", ".##..##.", ".##..##.", ".#####..", ".##.....", ".##....."}}, // p
    {0x71, {"........", "........", "..#####.", ".##..##.", ".##..##.", "..#####.", ".....##.", ".....##."}}, // q
    {0x72, {"........", "........", ".#####..", ".##..##.", ".##.....", ".##.....", ".##.....", "........"}}, // r
    {0x73, {"........", "........", "..#####.", ".##.....", "..####..", ".....##.", ".#####..", "........"}}, // s
    {0x74, {"........", "..##....", ".######.", "..##....", "..##....", "..##....", "...###..", "........"}}, // t
    {0x75, {"........", "........", ".##..##.", ".##..##.", ".##..##.", ".##..##.", "..#####.", "........"}}, // u
    {0x76, {"........", "........", ".##..##.", ".##..##.", ".##..##.", "..####..", "...##...", "........"}}, // v
    {0x77, {"........", "........", ".##...##", ".##.#.##", ".#######", "..##.##.", "..##.##.", "........"}}, // w
    {0x78, {"........", "........", ".##..##.", "..####..", "...##...", "..####..", ".##..##.", "........"}}, // x
    {0x79, {"........", "........", ".##..##.", ".##..##.", ".##..##.", "..#####.", "....##..", ".####..."}}, // y
    {0x7A, {"........", "........", ".######.", "....##..", "...##...", "..##....", ".######.", "........"}}, // z
    {0x7C, {"...##...", "...##...", "...##...", "...##...", "...##...", "...##...", "...##...", "...##..."}}, // |
};

bool os_build(uint8_t rom[0x4000], char *err, size_t errlen) {
    memset(rom, 0, 0x4000);
    uint8_t *font = rom + 0x2000;  // $E000
    for (size_t i = 0; i < sizeof glyphs / sizeof glyphs[0]; i++) {
        for (int r = 0; r < 8; r++) {
            uint8_t bits = 0;
            for (int b = 0; b < 8; b++) bits = bits << 1 | (glyphs[i].rows[r][b] == '#');
            font[glyphs[i].code * 8 + r] = bits;
        }
    }
    return asm6502(source, rom, 0xC000, 0x4000, err, errlen);
}
