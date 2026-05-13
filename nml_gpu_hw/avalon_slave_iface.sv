// avalon_slave_iface.sv
// Avalon-MM lightweight slave for nml_gpu.
// 16 KB address window (14-bit byte address).
//
// Register map (byte offsets):
//   0x0000  CTRL         R/W  bit0=ENABLE, bit1=SWAP (auto-clear), bit2=HUD_ON
//   0x0004  STATUS       R    bit0=VBLANK, bit1=SWAP_PENDING, [15:8]=frame_ctr
//   0x0008  BG_SCROLL    R/W  [15:0]=scroll_x signed, [31:16]=scroll_y signed
//   0x000C  IRQ_MASK     R/W  bit0=VBLANK IRQ enable (Phase 2; wired 0 here)
//   0x0010  PLAYER_POS   R/W  [15:0]=px, [31:16]=py
//   0x0014  PLAYER_STATS R/W  [7:0]=hp, [15:8]=wave, [31:16]=level
//   0x0018  SCORE        R/W  32-bit score
//   0x001C  KILL_COUNT   R/W  32-bit kills
//   0x0020  HUD_AUX      R/W  [7:0]=ammo BCD (2 digits), [11:8]=art charges
//                             (4-bit BCD), [15:12]=gas charges (4-bit BCD)
//                             0x0024-0x002F reserved (region_aux returns 0)
//   0x0100-0x01FF  Sprite table (32 x 8 B = 256 B); SW reads return shadow
//   0x0400-0x07FF  Palette     (256 x 4 B = 1 KB)
//   0x1000-0x22BF  Tile map    (4800 B)
//
// Writes land in shadow immediately.  Sprite table commits on next VSYNC
// after CTRL.SWAP=1. Palette and tile map use a per-region dirty flag.

module avalon_slave_iface (
    input  logic        clk,
    input  logic        reset_n,

    // Avalon-MM slave
    input  logic [13:0] avs_address,    // byte address within 16 KB window
    input  logic        avs_read,
    input  logic        avs_write,
    input  logic [31:0] avs_writedata,
    input  logic [3:0]  avs_byteenable,
    output logic [31:0] avs_readdata,
    output logic        avs_waitrequest,

    // Status inputs from rest of design
    input  logic        vblank_in,
    input  logic        swap_pending_in,
    input  logic [7:0]  frame_ctr_in,

    // Control outputs
    output logic        ctrl_enable,
    output logic        ctrl_swap_req,   // one-cycle pulse on SWAP write
    output logic        ctrl_hud_on,
    output logic [31:0] bg_scroll,

    // Memory write fan-out
    output logic        sprtab_we,
    output logic        palette_we,
    output logic        tilemap_we,
    output logic [12:0] mem_waddr,      // max of the three regions
    output logic [31:0] mem_wdata,

    // Read-back from shadow RAMs (for SW reads of sprite/palette/tilemap)
    input  logic [31:0] sprtab_rdata_sw,
    input  logic [31:0] palette_rdata_sw,
    input  logic [31:0] tilemap_rdata_sw,

    // Player state registers -> HUD overlay
    output logic [31:0] player_pos,
    output logic [31:0] player_stats,
    output logic [31:0] score_reg,
    output logic [31:0] kill_count,

    // HUD auxiliary register (ammo + ability charges) -> compositor
    output logic [31:0] hud_aux
);

    // -----------------------------------------------------------------------
    // Control registers
    // -----------------------------------------------------------------------
    logic [31:0] ctrl_reg;
    logic [31:0] bg_scroll_reg;
    logic [31:0] irq_mask_reg;
    logic [31:0] player_pos_reg;
    logic [31:0] player_stats_reg;
    logic [31:0] score_reg_int;
    logic [31:0] kill_count_reg;
    logic [31:0] hud_aux_reg;

    assign ctrl_enable = ctrl_reg[0];
    assign ctrl_hud_on = ctrl_reg[2];
    assign bg_scroll   = bg_scroll_reg;
    assign player_pos  = player_pos_reg;
    assign player_stats= player_stats_reg;
    assign score_reg   = score_reg_int;
    assign kill_count  = kill_count_reg;
    assign hud_aux     = hud_aux_reg;

    // ctrl_swap_req: SWAP request held for several clk cycles so the pix_clk
    // domain (running at clk/2) reliably samples it. A single-cycle pulse
    // (~20 ns at 50 MHz) is too narrow for the 40 ns pix_clk to catch through
    // a 2-FF synchronizer; widening it here removes the CDC race.
    logic [2:0] swap_req_cnt;
    assign ctrl_swap_req = (swap_req_cnt != 3'd0);

    // -----------------------------------------------------------------------
    // Address decode
    // -----------------------------------------------------------------------
    // Regions (byte addresses):
    //   [13:4] == 0 -> scalar registers (0x000-0x00F base)
    //   Specifically: 0x00-0x0F is ctrl/status, 0x10-0x1F is player state
    //   0x0100-0x01FF -> sprite table
    //   0x0400-0x07FF -> palette
    //   0x1000-0x22BF -> tile map

    logic region_ctrl;       // 0x0000-0x000F
    logic region_player;     // 0x0010-0x001F
    logic region_aux;        // 0x0020-0x002F (HUD_AUX + reserved siblings)
    logic region_sprtab;     // 0x0100-0x01FF
    logic region_palette;    // 0x0400-0x07FF
    logic region_tilemap;    // 0x1000-0x22BF

    assign region_ctrl    = (avs_address[13:4] == 10'h000);
    assign region_player  = (avs_address[13:4] == 10'h001);
    assign region_aux     = (avs_address[13:4] == 10'h002);
    assign region_sprtab  = (avs_address[13:8] == 6'h01);          // 0x100-0x1FF
    assign region_palette = (avs_address[13:10] == 4'h1) &&
                            (avs_address[9:8]  != 2'b00 ||
                             avs_address[13:10] == 4'h1);           // 0x400-0x7FF
    assign region_tilemap = (avs_address >= 14'h1000) &&
                            (avs_address <  14'h22C0);              // 0x1000-0x22BF

    // -----------------------------------------------------------------------
    // Read latency: RAM regions (sprtab/palette/tilemap) drive their
    // *_rdata_sw signals via an always_ff register, so the data is valid one
    // clk after avs_read goes high. Scalar registers (CTRL/STATUS/...) are
    // driven combinationally. Hold avs_waitrequest high for the first cycle
    // of any RAM-region read so the master captures the registered data on
    // the cycle it actually arrives, not the previous cycle's idle latch.
    // -----------------------------------------------------------------------
    logic ram_read_active;
    logic ram_read_seen;
    assign ram_read_active = avs_read && (region_sprtab || region_palette || region_tilemap);
    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n)            ram_read_seen <= 1'b0;
        else if (!ram_read_active) ram_read_seen <= 1'b0;
        else                       ram_read_seen <= 1'b1;
    end
    assign avs_waitrequest = ram_read_active && !ram_read_seen;

    // Sprite table: 32 entries x 8B; SW address [7:2] = word index (0-63 words -> 32 entries x 2 words)
    // Palette:      256 entries x 4B; SW address [9:2] = entry index
    // Tile map:     4800B; SW address [12:0] within region

    // Memory address and write-enable fan-out.
    //
    // The slave is configured with Address Units = SYMBOLS in Platform
    // Designer, so avs_address[13:0] is a byte address. The downstream
    // memory consumers in nml_gpu.sv expect a *word* index for sprtab and
    // palette, so we shift by 2 here. mem_waddr is computed whenever a
    // region matches (read or write) so sprtab_rdata_sw / palette_rdata_sw /
    // tilemap_rdata_sw return the correct location on devmem2 reads, not
    // just on writes.
    always_comb begin
        sprtab_we  = 1'b0;
        palette_we = 1'b0;
        tilemap_we = 1'b0;
        mem_waddr  = '0;
        mem_wdata  = avs_writedata;

        if (region_sprtab) begin
            mem_waddr = {7'b0, avs_address[7:2]};  // 6-bit word index (0..63 -> 32 sprites x 2 words)
            if (avs_write) sprtab_we = 1'b1;
        end else if (region_palette) begin
            mem_waddr = {5'b0, avs_address[9:2]};  // 8-bit palette entry index
            if (avs_write) palette_we = 1'b1;
        end else if (region_tilemap) begin
            // Byte offset within the tile-map region (avs_address - 0x1000).
            // Packed as {avs_address[13], avs_address[11:0]}: for addresses
            // 0x1000-0x1FFF, bit 13=0 and bit 12=1 so this gives 0x000-0xFFF.
            // For 0x2000-0x22BF, bit 13=1 and bit 12=0 so this gives 0x1000-
            // 0x12BF. Truncating to [12:0] (the old behavior) aliased the
            // upper half on top of the lower half and clobbered rows 0..8.
            mem_waddr = {avs_address[13], avs_address[11:0]};
            if (avs_write) tilemap_we = 1'b1;
        end
    end

    // -----------------------------------------------------------------------
    // Write logic for scalar registers
    // -----------------------------------------------------------------------
    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            ctrl_reg         <= 32'h0000_0001;  // ENABLE=1 at reset
            bg_scroll_reg    <= '0;
            irq_mask_reg     <= '0;
            player_pos_reg   <= '0;
            player_stats_reg <= '0;
            score_reg_int    <= '0;
            kill_count_reg   <= '0;
            hud_aux_reg      <= '0;
            swap_req_cnt     <= 3'd0;
        end else begin
            // SWAP request: count down each cycle until 0. A new SWAP write
            // reloads the counter (held high for 7 clk cycles ~140 ns, well
            // over the 80 ns window needed by a 2-FF synchroniser at 25 MHz).
            if (swap_req_cnt != 3'd0) swap_req_cnt <= swap_req_cnt - 3'd1;

            if (avs_write) begin
                if (region_ctrl) begin
                    case (avs_address[3:2])
                        2'h0: begin
                            ctrl_reg <= avs_writedata & 32'hFFFFFF07;
                            if (avs_writedata[1]) begin
                                swap_req_cnt <= 3'd7;     // hold ctrl_swap_req
                                ctrl_reg[1]  <= 1'b0;     // SWAP auto-clears
                            end
                        end
                        2'h1: ; // STATUS is read-only
                        2'h2: bg_scroll_reg <= avs_writedata;
                        2'h3: irq_mask_reg  <= avs_writedata;
                    endcase
                end else if (region_player) begin
                    case (avs_address[3:2])
                        2'h0: player_pos_reg   <= avs_writedata;
                        2'h1: player_stats_reg <= avs_writedata;
                        2'h2: score_reg_int    <= avs_writedata;
                        2'h3: kill_count_reg   <= avs_writedata;
                    endcase
                end else if (region_aux) begin
                    case (avs_address[3:2])
                        2'h0: hud_aux_reg <= avs_writedata;
                        default: ; // 0x24-0x2F reserved, writes ignored
                    endcase
                end
            end
        end
    end

    // -----------------------------------------------------------------------
    // Read logic (combinational, single-cycle)
    // -----------------------------------------------------------------------
    always_comb begin
        avs_readdata = 32'h0000_0000;

        if (avs_read) begin
            if (region_ctrl) begin
                case (avs_address[3:2])
                    2'h0: avs_readdata = ctrl_reg;
                    2'h1: avs_readdata = {16'h0, frame_ctr_in, 6'h0, swap_pending_in, vblank_in};
                    2'h2: avs_readdata = bg_scroll_reg;
                    2'h3: avs_readdata = irq_mask_reg;
                endcase
            end else if (region_player) begin
                case (avs_address[3:2])
                    2'h0: avs_readdata = player_pos_reg;
                    2'h1: avs_readdata = player_stats_reg;
                    2'h2: avs_readdata = score_reg_int;
                    2'h3: avs_readdata = kill_count_reg;
                endcase
            end else if (region_aux) begin
                case (avs_address[3:2])
                    2'h0:    avs_readdata = hud_aux_reg;
                    default: avs_readdata = 32'h0;
                endcase
            end else if (region_sprtab) begin
                avs_readdata = sprtab_rdata_sw;
            end else if (region_palette) begin
                avs_readdata = palette_rdata_sw;
            end else if (region_tilemap) begin
                avs_readdata = tilemap_rdata_sw;
            end
        end
    end

endmodule
