// de1soc_top.sv -- DE1-SoC top level for the No Man's Land smoke-test bitstream.
//
// PURPOSE
//   FPGA-fabric-only top level. No HPS, no Qsys, no SD card. The bitstream
//   alone produces a complete VGA frame using the data pre-loaded into
//   sprite_rom.hex / tile_rom.hex / palette.hex / sprite_table.hex via
//   $readmemh inside nml_gpu. This is the fastest way to confirm the entire
//   compositor pipeline works on real silicon.
//
// WHEN TO USE
//   - Today: this top, to confirm 25 MHz pixel clock + VGA + nml_gpu render.
//   - Later (full game): replace this top with a Qsys-integrated wrapper that
//     exposes nml_gpu's Avalon slave on the HPS-to-FPGA Lightweight bridge.
//     See SETUP.md "Phase 2: HPS integration" for that workflow.
//
// PINS / BOARD CONTROLS
//   CLOCK_50      50 MHz oscillator
//   KEY[0]        Active-low reset (push to reset everything)
//   SW[9:0]       Reserved for future use (currently mirrored to LEDR)
//   LEDR[9:0]    Status:
//                  LEDR[0] = always 1  (FPGA powered)
//                  LEDR[1] = mirrors VGA visible region (lit during active video)
//                  LEDR[9:2] = SW[9:2] passthrough
//
// VGA pin assignments (DE1-SoC ADV7123 DAC) live in nml_gpu.qsf.

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
