module nml_gpu (
    // Avalon-MM slave (Lightweight bridge, 50 MHz)
    input  logic        clk,
    input  logic        reset_n,
    input  logic [13:0] avs_address,    // 16 KB window, byte addr
    input  logic        avs_read,
    input  logic        avs_write,
    input  logic [31:0] avs_writedata,
    input  logic [3:0]  avs_byteenable,
    output logic [31:0] avs_readdata,
    output logic        avs_waitrequest, // tied 0; always single-cycle

    // VGA (driven by internal pix_clk derived from PLL)
    output logic [7:0]  vga_r,
    output logic [7:0]  vga_g,
    output logic [7:0]  vga_b,
    output logic        vga_hs,
    output logic        vga_vs,
    output logic        vga_blank_n,
    output logic        vga_sync_n,
    output logic        vga_clk
);

    // -----------------------------------------------------------
    // Internal Signals & Wires
    // -----------------------------------------------------------
    logic        pix_clk;
    logic [9:0]  x, y;
    logic        visible, hsync, vsync, vblank, hblank;
    
    // Register file outputs
    logic        ctrl_enable, ctrl_swap_req, ctrl_hud_on;
    logic [31:0] bg_scroll;
    logic [31:0] player_pos, player_stats, score, kill_count;
    
    // Memory write fan-out
    logic [12:0] mem_waddr;
    logic        sprtab_we, palette_we, tilemap_we;
    logic [31:0] mem_wdata;

    // Sprite Evaluation -> Compositor
    logic [4:0]  sprtab_raddr;
    logic [63:0] sprtab_rdata;
    logic [7:0]  active_mask;
    logic [63:0] line_sprites [0:7];

    // ... (Memory and Submodule Instantiations will go here) ...
    
    assign avs_waitrequest = 1'b0; // Single-cycle access

endmodule
