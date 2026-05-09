// pll_25mhz.v
// 50 MHz -> 25 MHz pixel clock for the VGA pipeline.
//
// Pragmatic divide-by-2 implementation suitable for the smoke-test bitstream.
// Quartus will route the divided output through a global clock buffer because
// of the CLOCK_BUFFER attribute, so downstream FF timing is OK for a stable
// 640x480@60 monitor sync.
//
// PRODUCTION upgrade path (recommended after first working bitstream): replace
// this module with an `altera_pll` IP generated in Platform Designer:
//   - Output 0: 25 MHz, 50% duty cycle
//   - Locked output unused (we tie reset directly)
// The instance port names below (refclk / rst / outclk_0) match the altera_pll
// defaults so swapping in the IP requires no edits in nml_gpu.sv.

module pll_25mhz (
    input  wire refclk,    // 50 MHz reference
    input  wire rst,       // active high reset
    output wire outclk_0   // 25 MHz pixel clock
);

    (* altera_attribute = "-name CLOCK_BUFFER ON" *)
    reg clk_div = 1'b0;

    always @(posedge refclk or posedge rst) begin
        if (rst) clk_div <= 1'b0;
        else     clk_div <= ~clk_div;
    end

    assign outclk_0 = clk_div;

endmodule
