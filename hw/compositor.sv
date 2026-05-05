module compositor (
    input  logic        pix_clk, reset_n,
    input  logic [9:0]  x, y,
    input  logic        visible,
    input  logic [15:0] scroll_x, scroll_y,

    // Tile map read
    output logic [12:0] tilemap_raddr,
    input  logic [7:0]  tilemap_rdata,

    // Tile ROM read
    output logic [11:0] tilerom_raddr,
    input  logic [7:0]  tilerom_rdata,

    // Line Buffer Interface (Simplification: assumes line buffer is already filled during HBLANK)
    output logic [9:0]  linebuf_raddr,
    input  logic [7:0]  linebuf_rdata, // Contains sprite palette index (0 = transparent)

    // Palette read
    output logic [7:0]  palette_raddr,
    input  logic [23:0] palette_rdata,

    // Pixel output
    output logic [23:0] rgb_out
);

    // --- PIPELINE STAGE 1: Address Calculation ---
    // Background is an 80x60 grid of 8x8 pixel tiles.
    logic [6:0] tile_col;
    logic [5:0] tile_row;
    
    assign tile_col = x[9:3]; // x / 8
    assign tile_row = y[9:3]; // y / 8
    
    // Tile map is 80 columns wide. Address = (row * 80) + col
    assign tilemap_raddr = (tile_row * 80) + tile_col;
    
    // We also read the sprite line buffer at the current X coordinate
    assign linebuf_raddr = x;

    // --- PIPELINE STAGE 2 & 3: ROM and Palette Fetch ---
    // Delay X and Y by 1 cycle to match the RAM read latency of the tilemap
    logic [2:0] x_delay, y_delay;
    logic [7:0] sprite_pixel_delay;
    logic       visible_delay1, visible_delay2;

    always_ff @(posedge pix_clk) begin
        x_delay <= x[2:0];
        y_delay <= y[2:0];
        sprite_pixel_delay <= linebuf_rdata; // Fetch the sprite pixel from the line buffer
        visible_delay1 <= visible;
        visible_delay2 <= visible_delay1;
    end

    // Now we have the tile ID from the tilemap. Calculate Tile ROM address.
    // Tile ROM blocks are 64 bytes (8x8). Address = (tile_id * 64) + (y * 8) + x
    assign tilerom_raddr = ({tilemap_rdata, 6'd0}) + ({y_delay, 3'd0}) + x_delay;

    // Priority MUX[cite: 111]: If sprite pixel is non-zero (transparent), it wins. Otherwise tile wins.
    always_comb begin
        if (sprite_pixel_delay != 8'd0) begin
            palette_raddr = sprite_pixel_delay;
        end else begin
            palette_raddr = tilerom_rdata;
        end
    end

    // --- PIPELINE STAGE 4: Final Output ---
    always_ff @(posedge pix_clk or negedge reset_n) begin
        if (!reset_n) begin
            rgb_out <= 24'd0;
        end else begin
            // Wait for the pipeline delays to resolve before outputting colors
            if (visible_delay2) begin
                rgb_out <= palette_rdata; // [cite: 111]
            end else begin
                rgb_out <= 24'd0; // Black during blanking
            end
        end
    end

endmodule
