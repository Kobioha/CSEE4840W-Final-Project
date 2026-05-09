module sprite_eval (
    input  logic        pix_clk, reset_n,
    input  logic [9:0]  next_scanline,
    input  logic        eval_strobe,
    
    // Sprite table read port
    output logic [4:0]  sprtab_raddr,
    input  logic [63:0] sprtab_rdata,
    
    // Output: up to 8 active sprites for the next line
    output logic [63:0] line_sprites [0:7],
    output logic [7:0]  active_mask,
    // High for one cycle when DONE finishes latching the sorted list.
    output logic        eval_done
);

    typedef enum logic [1:0] {IDLE, FETCH, EVAL, DONE} state_t;
    state_t state, next_state;

    logic [5:0] sprite_idx; // 0 to 32
    
    // Internal tracking for the top 8 sprites
    logic [63:0] top_sprites [0:7];
    logic [2:0]  top_prios   [0:7];
    logic        top_valid   [0:7];

    // Sprite data unpacking (based on struct layout)
    logic signed [15:0] spr_y;
    logic [2:0]         spr_prio;
    logic               spr_active;
    
    assign spr_y      = sprtab_rdata[31:16];
    assign spr_prio   = sprtab_rdata[46:44];
    assign spr_active = sprtab_rdata[47];

    // Intersection check: Y is signed, next_scanline is unsigned. 
    // All sprites are 16x16 in baseline.
    logic intersects;
    assign intersects = (spr_active) && 
                        (next_scanline >= spr_y) && 
                        (next_scanline < (spr_y + 16));

    always_ff @(posedge pix_clk or negedge reset_n) begin
        if (!reset_n) begin
            state <= IDLE;
            sprite_idx <= 6'd0;
            active_mask <= 8'd0;
            eval_done <= 1'b0;
            for (int i = 0; i < 8; i++) begin
                top_valid[i] <= 1'b0;
                line_sprites[i] <= 64'd0;
            end
        end else begin
            eval_done <= 1'b0;
            case (state)
                IDLE: begin
                    if (eval_strobe) begin
                        state <= FETCH;
                        sprite_idx <= 6'd0;
                        // Clear the tracking arrays for the new scanline
                        for (int i = 0; i < 8; i++) top_valid[i] <= 1'b0;
                    end
                end

                FETCH: begin
                    sprtab_raddr <= sprite_idx[4:0];
                    state <= EVAL;
                end

                EVAL: begin
                    if (intersects) begin
                        // Hardware Insertion Sort: Find where this sprite belongs based on priority (0 is highest)
                        // This block checks all 8 slots simultaneously and shifts lower-priority sprites down.
                        logic inserted;
                        inserted = 1'b0;
                        
                        for (int i = 0; i < 8; i++) begin
                            if (!inserted) begin
                                if (!top_valid[i] || (spr_prio < top_prios[i])) begin
                                    // Insert here
                                    top_sprites[i] <= sprtab_rdata;
                                    top_prios[i]   <= spr_prio;
                                    top_valid[i]   <= 1'b1;
                                    inserted       = 1'b1;
                                    
                                    // Shift the rest down (drops the 8th item if array is full)
                                    for (int j = 7; j > i; j--) begin
                                        top_sprites[j] <= top_sprites[j-1];
                                        top_prios[j]   <= top_prios[j-1];
                                        top_valid[j]   <= top_valid[j-1];
                                    end
                                end
                            end
                        end
                    end

                    sprite_idx <= sprite_idx + 1;
                    if (sprite_idx == 6'd31) begin
                        state <= DONE;
                    end else begin
                        state <= FETCH;
                    end
                end

                DONE: begin
                    // Latch the final sorted list into the output ports
                    for (int i = 0; i < 8; i++) begin
                        line_sprites[i] <= top_sprites[i];
                        active_mask[i]  <= top_valid[i];
                    end
                    eval_done <= 1'b1;
                    state    <= IDLE;
                end
            endcase
        end
    end
endmodule
