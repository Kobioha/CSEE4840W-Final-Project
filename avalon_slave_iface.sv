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

    // Player state registers → HUD overlay
    output logic [31:0] player_pos,
    output logic [31:0] player_stats,
    output logic [31:0] score_reg,
    output logic [31:0] kill_count
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

    assign ctrl_enable = ctrl_reg[0];
    assign ctrl_hud_on = ctrl_reg[2];
    assign bg_scroll   = bg_scroll_reg;
    assign player_pos  = player_pos_reg;
    assign player_stats= player_stats_reg;
    assign score_reg   = score_reg_int;
    assign kill_count  = kill_count_reg;

    // ctrl_swap_req: one-cycle pulse when SW writes SWAP bit
    logic swap_req_r;
    assign ctrl_swap_req = swap_req_r;

    // -----------------------------------------------------------------------
    // Address decode
    // -----------------------------------------------------------------------
    // Regions (byte addresses):
    //   [13:4] == 0 → scalar registers (0x000-0x00F base)
    //   Specifically: 0x00-0x0F is ctrl/status, 0x10-0x1F is player state
    //   0x0100-0x01FF → sprite table
    //   0x0400-0x07FF → palette
    //   0x1000-0x22BF → tile map

    logic region_ctrl;       // 0x0000-0x000F
    logic region_player;     // 0x0010-0x001F
    logic region_sprtab;     // 0x0100-0x01FF
    logic region_palette;    // 0x0400-0x07FF
    logic region_tilemap;    // 0x1000-0x22BF

    assign region_ctrl    = (avs_address[13:4] == 10'h000);
    assign region_player  = (avs_address[13:4] == 10'h001);
    assign region_sprtab  = (avs_address[13:8] == 6'h01);          // 0x100-0x1FF
    assign region_palette = (avs_address[13:10] == 4'h1) &&
                            (avs_address[9:8]  != 2'b00 ||
                             avs_address[13:10] == 4'h1);           // 0x400-0x7FF
    assign region_tilemap = (avs_address[13] == 1'b1);              // 0x1000-0x3FFF

    // Sprite table: 32 entries × 8B; SW address [7:2] = word index (0-63 words → 32 entries × 2 words)
    // Palette:      256 entries × 4B; SW address [9:2] = entry index
    // Tile map:     4800B; SW address [12:0] within region

    // Memory write addresses (word granularity where applicable)
    always_comb begin
        sprtab_we  = 1'b0;
        palette_we = 1'b0;
        tilemap_we = 1'b0;
        mem_waddr  = '0;
        mem_wdata  = avs_writedata;

        if (avs_write) begin
            if (region_sprtab) begin
                sprtab_we = 1'b1;
                mem_waddr = {6'b0, avs_address[6:0]};  // byte address within sprite table
            end else if (region_palette) begin
                palette_we = 1'b1;
                mem_waddr  = {5'b0, avs_address[9:2]}; // entry index (word addressed)
            end else if (region_tilemap) begin
                tilemap_we = 1'b1;
                mem_waddr  = avs_address[12:0];
            end
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
            swap_req_r       <= 1'b0;
        end else begin
            swap_req_r <= 1'b0;  // auto-clear

            if (avs_write) begin
                if (region_ctrl) begin
                    case (avs_address[3:2])
                        2'h0: begin
                            ctrl_reg <= avs_writedata & 32'hFFFFFF07;
                            if (avs_writedata[1]) begin
                                swap_req_r   <= 1'b1;
                                ctrl_reg[1]  <= 1'b0;  // SWAP auto-clears
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
