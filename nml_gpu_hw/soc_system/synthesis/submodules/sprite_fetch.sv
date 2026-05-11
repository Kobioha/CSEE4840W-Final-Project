module sprite_fetch (
    input  logic        pix_clk, reset_n,
    input  logic        hblank,
    input  logic [9:0]  next_scanline,
    
    // Inputs from sprite_eval
    input  logic        eval_done,       // Pulses high when sprite_eval finishes
    input  logic [7:0]  active_mask,
    input  logic [63:0] line_sprites [0:7], // 0 is highest priority, 7 is lowest
    
    // Sprite ROM read port
    output logic [13:0] sprrom_raddr,
    input  logic [7:0]  sprrom_rdata,
    
    // Line Buffer write port (to the "Fill" buffer)
    output logic [9:0]  linebuf_waddr,
    output logic [7:0]  linebuf_wdata,
    output logic        linebuf_we
);

    typedef enum logic [2:0] {IDLE, CLEAR_BUF, PROCESS_SPRITE, READ_PIXEL, WAIT_PIXEL, WRITE_PIXEL} state_t;
    state_t state;

    logic [3:0] sprite_idx; // 0 to 7 (iterating backwards from 7 down to 0)
    logic [4:0] pixel_u;    // 0 to 15 (columns within the 16x16 sprite)
    logic [9:0] clear_idx;  // 0 to 639 for clearing the buffer
    
    // Unpacked sprite data for the CURRENT sprite being processed
    logic [63:0] current_sprite;
    logic signed [15:0] spr_x, spr_y;
    logic [5:0]  spr_id;
    logic        spr_hflip;
    
    assign current_sprite = line_sprites[sprite_idx[2:0]];
    assign spr_x     = current_sprite[15:0];
    assign spr_y     = current_sprite[31:16];
    assign spr_id    = current_sprite[37:32];
    assign spr_hflip = current_sprite[42];

    // Calculate which row (v) of the sprite we are drawing
    logic [3:0] pixel_v;
    assign pixel_v = next_scanline - spr_y;

    // Calculate target X screen coordinate
    logic signed [15:0] target_x;
    assign target_x = spr_x + pixel_u;

    always_ff @(posedge pix_clk or negedge reset_n) begin
        if (!reset_n) begin
            state <= IDLE;
            linebuf_we <= 1'b0;
            sprite_idx <= 4'd7;
            clear_idx <= 10'd0;
        end else begin
            // Default to not writing
            linebuf_we <= 1'b0;

            case (state)
                IDLE: begin
                    // When HBLANK starts, first thing we do is clear the fill buffer
                    if (hblank) begin
                        state <= CLEAR_BUF;
                        clear_idx <= 10'd0;
                    end
                end

                CLEAR_BUF: begin
                    // Sweep across the 640 pixels and write 0 (transparent)
                    linebuf_waddr <= clear_idx;
                    linebuf_wdata <= 8'd0;
                    linebuf_we    <= 1'b1;
                    
                    if (clear_idx == 10'd639) begin
                        // Wait here until sprite_eval is done evaluating the next line
                        if (eval_done) begin
                            state <= PROCESS_SPRITE;
                            sprite_idx <= 4'd7; // Start with lowest priority (index 7)
                        end else begin
                            linebuf_we <= 1'b0;
                        end
                    end else begin
                        clear_idx <= clear_idx + 10'd1;
                    end
                end

                PROCESS_SPRITE: begin
                    if (sprite_idx > 4'd7) begin // Underflowed past 0 (meaning we did 7 down to 0)
                        state <= IDLE;           // We are done with all sprites!
                    end else if (active_mask[sprite_idx[2:0]]) begin
                        // This sprite is active! Let's fetch its pixels.
                        pixel_u <= 5'd0;
                        state <= READ_PIXEL;
                    end else begin
                        // Skip inactive sprites
                        sprite_idx <= sprite_idx - 4'd1;
                    end
                end

                READ_PIXEL: begin
                    if (pixel_u == 5'd16) begin
                        // Finished all 16 columns for this sprite, move to next highest priority
                        sprite_idx <= sprite_idx - 4'd1;
                        state <= PROCESS_SPRITE;
                    end else begin
                        // Address = (sprite_id * 256) + (v * 16) + u
                        logic [3:0] actual_u;
                        actual_u = spr_hflip ? (4'd15 - pixel_u[3:0]) : pixel_u[3:0];

                        sprrom_raddr <= ({spr_id, 8'd0}) + ({pixel_v, 4'd0}) + actual_u;
                        // sprrom_raddr update + ROM read register together cost 2 cycles,
                        // so insert a WAIT_PIXEL bubble before sampling sprrom_rdata.
                        state <= WAIT_PIXEL;
                    end
                end

                WAIT_PIXEL: begin
                    // ROM read in flight; sprrom_rdata becomes valid next cycle.
                    state <= WRITE_PIXEL;
                end

                WRITE_PIXEL: begin
                    // If pixel isn't transparent (0) and is on screen, write it!
                    if (sprrom_rdata != 8'd0 && target_x >= 0 && target_x < 640) begin
                        linebuf_waddr <= target_x[9:0];
                        linebuf_wdata <= sprrom_rdata;
                        linebuf_we    <= 1'b1;
                    end

                    pixel_u <= pixel_u + 5'd1;
                    state <= READ_PIXEL;
                end
            endcase
        end
    end
endmodule
