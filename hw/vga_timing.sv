module vga_timing (
    input  logic       pix_clk, reset_n,
    output logic [9:0] x,       // 0..799 (640 visible + porches)
    output logic [9:0] y,       // 0..524 (480 visible + porches)
    output logic       visible,
    output logic       hsync, vsync,
    output logic       vblank, hblank
);

    // Standard 640x480 @ 60Hz parameters
    localparam H_ACTIVE      = 640;
    localparam H_FRONT_PORCH = 16;
    localparam H_SYNC_PULSE  = 96;
    localparam H_BACK_PORCH  = 48;
    localparam H_TOTAL       = 800;

    localparam V_ACTIVE      = 480;
    localparam V_FRONT_PORCH = 10;
    localparam V_SYNC_PULSE  = 2;
    localparam V_BACK_PORCH  = 33;
    localparam V_TOTAL       = 525;

    // Internal counters
    logic [9:0] h_count;
    logic [9:0] v_count;

    // Assign outputs directly from counters
    assign x = h_count;
    assign y = v_count;

    always_ff @(posedge pix_clk or negedge reset_n) begin
        if (!reset_n) begin
            h_count <= 10'd0;
            v_count <= 10'd0;
        end else begin
            if (h_count == H_TOTAL - 1) begin
                h_count <= 10'd0;
                if (v_count == V_TOTAL - 1) begin
                    v_count <= 10'd0;
                end else begin
                    v_count <= v_count + 10'd1;
                end
            end else begin
                h_count <= h_count + 10'd1;
            end
        end
    end

    // Sync generation (Active Low for standard 640x480 VGA)
    assign hsync = ~(h_count >= (H_ACTIVE + H_FRONT_PORCH) && 
                     h_count <  (H_ACTIVE + H_FRONT_PORCH + H_SYNC_PULSE));
                     
    assign vsync = ~(v_count >= (V_ACTIVE + V_FRONT_PORCH) && 
                     v_count <  (V_ACTIVE + V_FRONT_PORCH + V_SYNC_PULSE));

    // Blanking logic
    assign hblank = (h_count >= H_ACTIVE);
    assign vblank = (v_count >= V_ACTIVE);
    
    // Visible region is when neither horizontal nor vertical blanking is active
    assign visible = !hblank && !vblank;

endmodule
