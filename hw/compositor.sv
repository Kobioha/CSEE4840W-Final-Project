module compositor (
    input  logic        pix_clk, reset_n,
    input  logic [9:0]  x, y,
    input  logic        visible,
    input  logic [15:0] scroll_x, scroll_y,

    // HUD overlay inputs (per DESIGN.md §4.4)
    input  logic        hud_on,
    input  logic [31:0] player_stats,    // [7:0] hp (binary 0..100), [15:8] wave BCD, [31:16] level
    input  logic [31:0] score_reg,       // [23:0] 6-digit BCD score; [31:24] reserved

    // Tile map read
    output logic [12:0] tilemap_raddr,
    input  logic [7:0]  tilemap_rdata,

    // Tile ROM read (shared between background tiles and HUD digit glyphs;
    // never collide because the HUD-text region is a disjoint y-slice).
    output logic [11:0] tilerom_raddr,
    input  logic [7:0]  tilerom_rdata,

    // Sprite line buffer (already filled during HBLANK)
    output logic [9:0]  linebuf_raddr,
    input  logic [7:0]  linebuf_rdata,   // sprite palette index, 0 = transparent

    // Palette read
    output logic [7:0]  palette_raddr,
    input  logic [23:0] palette_rdata,

    output logic [23:0] rgb_out
);

    // ===== HUD layout (DESIGN.md §4.4) =====
    localparam int HUD_H_END        = 16;
    localparam int HP_X_END         = 192;
    localparam int WAVE_X_START     = 312;   // 256 + (128-16)/2 = centered 2 digits in [256..383]
    localparam int WAVE_X_END       = 328;
    localparam int SCORE_X_START    = 520;   // 448 + (192-48)/2 = centered 6 digits in [448..639]
    localparam int SCORE_X_END      = 568;
    localparam int TEXT_Y_START     = 8;
    localparam int DIGIT_TILE_BASE  = 48;    // tile ROM slots 48..57 = digit glyphs 0..9

    localparam logic [23:0] RGB_HP_FULL  = 24'h00FF00;  // green
    localparam logic [23:0] RGB_HP_EMPTY = 24'h800000;  // dark red
    localparam logic [23:0] RGB_HUD_FG   = 24'hFFFFFF;  // white digit pixels
    localparam logic [23:0] RGB_HUD_BG   = 24'h202020;  // dark gray fill

    // ===== Stage 1: combinational region + digit selection from x, y =====
    wire in_hud_strip = hud_on && (y < HUD_H_END);
    wire in_hp_bar    = in_hud_strip && (x < HP_X_END);
    wire in_wave_text = in_hud_strip && (y >= TEXT_Y_START)
                                     && (x >= WAVE_X_START)  && (x < WAVE_X_END);
    wire in_score_text= in_hud_strip && (y >= TEXT_Y_START)
                                     && (x >= SCORE_X_START) && (x < SCORE_X_END);
    wire in_hud_text  = in_wave_text || in_score_text;
    wire in_hud_gap   = in_hud_strip && !in_hp_bar && !in_hud_text;

    // HP bar fill: hp*2 px clamped to 192 (hp ∈ 0..100, bar width 192 -> hp*1.92 ≈ hp*2).
    wire [8:0] hp_x2          = {1'b0, player_stats[7:0]} << 1;
    wire [8:0] hp_bar_width   = (hp_x2 > 9'd192) ? 9'd192 : hp_x2;
    wire       hp_filled      = ({1'b0, x[8:0]} < hp_bar_width);

    // Wave digit selection (BCD nibbles in player_stats[15:8])
    wire [9:0] wave_offset   = x - WAVE_X_START[9:0];
    wire       wave_digit_idx = wave_offset[3];        // 0 = left/tens, 1 = right/ones
    wire [3:0] wave_digit_bcd = wave_digit_idx ? player_stats[11:8]
                                                : player_stats[15:12];

    // Score digit selection (6 BCD nibbles in score_reg[23:0]; nibble 0 = ones).
    // Leftmost screen column shows highest digit, so screen idx 0 -> nibble 5.
    wire [9:0] score_offset      = x - SCORE_X_START[9:0];
    wire [2:0] score_digit_idx   = score_offset[5:3];   // 0..5
    wire [2:0] score_nibble_idx  = 3'd5 - score_digit_idx;
    logic [3:0] score_digit_bcd;
    always_comb begin
        case (score_nibble_idx)
            3'd0:    score_digit_bcd = score_reg[3:0];
            3'd1:    score_digit_bcd = score_reg[7:4];
            3'd2:    score_digit_bcd = score_reg[11:8];
            3'd3:    score_digit_bcd = score_reg[15:12];
            3'd4:    score_digit_bcd = score_reg[19:16];
            3'd5:    score_digit_bcd = score_reg[23:20];
            default: score_digit_bcd = 4'd0;
        endcase
    end

    wire [3:0] hud_digit_bcd = in_wave_text  ? wave_digit_bcd
                              : in_score_text ? score_digit_bcd
                                              : 4'd0;
    wire [5:0] hud_tile_id   = 6'(DIGIT_TILE_BASE) + hud_digit_bcd[3:0];
    wire [2:0] hud_tile_x    = x[2:0];        // 312 and 520 are 8-aligned
    wire [2:0] hud_tile_y    = y[2:0];

    // ===== Background tile lookup (unchanged) =====
    logic [6:0] tile_col;
    logic [5:0] tile_row;
    assign tile_col = x[9:3];
    assign tile_row = y[9:3];
    assign tilemap_raddr = (tile_row * 80) + tile_col;
    assign linebuf_raddr = x;

    // ===== Stage 1 -> Stage 2 latches =====
    logic       s2_in_hp_bar, s2_hp_filled;
    logic       s2_in_hud_text, s2_in_hud_gap;
    logic [5:0] s2_hud_tile_id;
    logic [2:0] s2_hud_tile_x, s2_hud_tile_y;
    logic [2:0] x_delay, y_delay;
    logic [7:0] sprite_pixel_delay;
    logic       visible_delay1;

    always_ff @(posedge pix_clk) begin
        s2_in_hp_bar       <= in_hp_bar;
        s2_hp_filled       <= hp_filled;
        s2_in_hud_text     <= in_hud_text;
        s2_in_hud_gap      <= in_hud_gap;
        s2_hud_tile_id     <= hud_tile_id;
        s2_hud_tile_x      <= hud_tile_x;
        s2_hud_tile_y      <= hud_tile_y;
        x_delay            <= x[2:0];
        y_delay            <= y[2:0];
        sprite_pixel_delay <= linebuf_rdata;
        visible_delay1     <= visible;
    end

    // ===== Stage 2: tilerom_raddr mux (HUD text vs background) =====
    wire [11:0] tilerom_addr_bg  = ({tilemap_rdata, 6'd0}) + ({y_delay, 3'd0}) + x_delay;
    wire [11:0] tilerom_addr_hud = ({s2_hud_tile_id, 6'd0}) + ({s2_hud_tile_y, 3'd0}) + s2_hud_tile_x;
    assign tilerom_raddr = s2_in_hud_text ? tilerom_addr_hud : tilerom_addr_bg;

    // ===== Stage 2 -> Stage 3 latches =====
    logic s3_in_hp_bar, s3_hp_filled;
    logic s3_in_hud_text, s3_in_hud_gap;
    logic visible_delay2;

    always_ff @(posedge pix_clk) begin
        s3_in_hp_bar    <= s2_in_hp_bar;
        s3_hp_filled    <= s2_hp_filled;
        s3_in_hud_text  <= s2_in_hud_text;
        s3_in_hud_gap   <= s2_in_hud_gap;
        visible_delay2  <= visible_delay1;
    end

    // ===== Stage 3: palette mux for non-HUD pixels =====
    // (HUD pixels bypass palette and use the hardcoded RGB constants below.)
    always_comb begin
        if (sprite_pixel_delay != 8'd0) palette_raddr = sprite_pixel_delay;
        else                            palette_raddr = tilerom_rdata;
    end

    // ===== Stage 4: final RGB =====
    // HUD layer always wins above sprites/tiles in the top strip.
    always_ff @(posedge pix_clk or negedge reset_n) begin
        if (!reset_n) begin
            rgb_out <= 24'd0;
        end else if (!visible_delay2) begin
            rgb_out <= 24'd0;
        end else if (s3_in_hp_bar) begin
            rgb_out <= s3_hp_filled ? RGB_HP_FULL : RGB_HP_EMPTY;
        end else if (s3_in_hud_text) begin
            // Digit tile uses palette index 0xFF for lit pixels, 0x00 for blank.
            rgb_out <= (tilerom_rdata != 8'd0) ? RGB_HUD_FG : RGB_HUD_BG;
        end else if (s3_in_hud_gap) begin
            rgb_out <= RGB_HUD_BG;
        end else begin
            rgb_out <= palette_rdata;
        end
    end

endmodule
