// de1soc_top.sv -- Phase 1 top for the smoke-test bitstream. No HPS, no Qsys.
// nml_gpu boots with its memories pre-loaded from the .hex files via
// $readmemh, so the bitstream alone draws a VGA frame. Replaced by
// soc_system_top in Phase 2. VGA pins are in nml_gpu.qsf.
//
// KEY[0]    active-low reset
// LEDR[0]   tied 1 (FPGA powered)
// LEDR[1]   high during VGA visible region
// LEDR[9:2] SW[9:2] passthrough

`timescale 1ns / 1ps

module de1soc_top (
    input  logic        CLOCK_50,
    input  logic [3:0]  KEY,
    input  logic [9:0]  SW,
    output logic [9:0]  LEDR,

    // VGA (24-bit DAC + sync)
    output logic [7:0]  VGA_R,
    output logic [7:0]  VGA_G,
    output logic [7:0]  VGA_B,
    output logic        VGA_HS,
    output logic        VGA_VS,
    output logic        VGA_BLANK_N,
    output logic        VGA_SYNC_N,
    output logic        VGA_CLK
);

    // Active-low reset on KEY[0]; KEY is active-low on the DE1-SoC.
    logic reset_n;
    assign reset_n = KEY[0];

    // Tie the Avalon slave off (no master in standalone mode).
    logic [31:0] avs_readdata;
    /* verilator lint_off UNUSEDSIGNAL */
    logic        avs_waitrequest_unused;
    /* verilator lint_on UNUSEDSIGNAL */

    nml_gpu gpu_inst (
        .clk            (CLOCK_50),
        .reset_n        (reset_n),

        // No master is driving the Avalon slave in standalone mode; tie inputs.
        .avs_address    (14'd0),
        .avs_read       (1'b0),
        .avs_write      (1'b0),
        .avs_writedata  (32'd0),
        .avs_byteenable (4'b0000),
        .avs_readdata   (avs_readdata),
        .avs_waitrequest(avs_waitrequest_unused),

        .vga_r          (VGA_R),
        .vga_g          (VGA_G),
        .vga_b          (VGA_B),
        .vga_hs         (VGA_HS),
        .vga_vs         (VGA_VS),
        .vga_blank_n    (VGA_BLANK_N),
        .vga_sync_n     (VGA_SYNC_N),
        .vga_clk        (VGA_CLK)
    );

    // Heartbeat / status LEDs so the team can confirm the bitstream is live
    // even before plugging in a monitor.
    assign LEDR[0]   = 1'b1;
    assign LEDR[1]   = VGA_BLANK_N;     // ON while pixels are visible
    assign LEDR[9:2] = SW[9:2];

endmodule
