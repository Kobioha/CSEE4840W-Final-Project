module compositor (
    input  logic        pix_clk, reset_n,
    input  logic [9:0]  x, y,
    input  logic        visible,
    input  logic [15:0] scroll_x, scroll_y,

    // HUD overlay inputs
    input  logic        hud_on,
    input  logic [31:0] player_stats,    // [7:0] hp, [15:8] wave BCD, [31:16] level
    input  logic [31:0] score_reg,       // [23:0] 6-digit BCD score
    input  logic [31:0] hud_aux,         // [7:0] ammo BCD, [11:8] art BCD, [15:12] gas BCD

    // Tile map read
    output logic [12:0] tilemap_raddr,
    input  logic [7:0]  tilemap_rdata,

    // Tile ROM read (shared between background tiles and HUD digit/letter
    // glyphs; never collide because the HUD-text region is a disjoint y-slice).
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

    // ===== HUD layout =====
    // 16-px strip across the top. Text glyphs are 8x8 and live at y=4..11 so
    // they're vertically centered in the strip.
    localparam int HUD_H_END        = 16;
    localparam int HP_X_END         = 192;

    // AMMO counter: "AMMO" label (4 letters) + 2-digit value.
    localparam int AMMO_LBL_X       = 200;
    localparam int AMMO_DIG_X       = 232;
    localparam int AMMO_END_X       = 248;

    // WAVE counter: "WAVE" label (4 letters) + existing 2-digit value.
    localparam int WAVE_LBL_X       = 280;
    localparam int WAVE_X_START     = 312;
    localparam int WAVE_X_END       = 328;

    // ART (artillery charges): "ART" label (3 letters) + 1-digit value.
    localparam int ART_LBL_X        = 360;
    localparam int ART_DIG_X        = 384;
    localparam int ART_END_X        = 392;

    // GAS (gas charges): "GAS" label (3 letters) + 1-digit value.
    localparam int GAS_LBL_X        = 408;
    localparam int GAS_DIG_X        = 432;
    localparam int GAS_END_X        = 440;

    // SCORE: "SCORE" label (5 letters) + existing 6-digit value at 520..567.
    localparam int SCORE_LBL_X      = 472;
    localparam int SCORE_LBL_END_X  = 512;
    localparam int SCORE_X_START    = 520;
    localparam int SCORE_X_END      = 568;

    localparam int TEXT_Y_START     = 4;   // glyphs span y=4..11 (centered in 16-px strip)
    localparam int TEXT_Y_END       = 12;
    localparam int DIGIT_TILE_BASE  = 48;  // tile ROM slots 48..57 = digit glyphs 0..9
    localparam int LETTER_TILE_BASE = 16;  // tile ROM slots 16..41 = letters A..Z

    localparam logic [23:0] RGB_HP_FULL  = 24'h00FF00;  // green
    localparam logic [23:0] RGB_HP_EMPTY = 24'h800000;  // dark red
    localparam logic [23:0] RGB_HUD_FG   = 24'hFFFFFF;  // white digit/letter pixels
    localparam logic [23:0] RGB_HUD_BG   = 24'h202020;  // dark gray HUD fill

    // ===== Stage 1: combinational region + tile-id selection =====
    wire in_hud_strip   = hud_on && (y < HUD_H_END);
    wire in_hp_bar      = in_hud_strip && (x < HP_X_END);
    wire text_y_active  = in_hud_strip && (y >= TEXT_Y_START) && (y < TEXT_Y_END);

    wire in_ammo_label  = text_y_active && (x >= AMMO_LBL_X)   && (x < AMMO_DIG_X);
    wire in_ammo_digits = text_y_active && (x >= AMMO_DIG_X)   && (x < AMMO_END_X);
    wire in_wave_label  = text_y_active && (x >= WAVE_LBL_X)   && (x < WAVE_X_START);
    wire in_wave_text   = text_y_active && (x >= WAVE_X_START) && (x < WAVE_X_END);
    wire in_art_label   = text_y_active && (x >= ART_LBL_X)    && (x < ART_DIG_X);
    wire in_art_digit   = text_y_active && (x >= ART_DIG_X)    && (x < ART_END_X);
    wire in_gas_label   = text_y_active && (x >= GAS_LBL_X)    && (x < GAS_DIG_X);
    wire in_gas_digit   = text_y_active && (x >= GAS_DIG_X)    && (x < GAS_END_X);
    wire in_score_label = text_y_active && (x >= SCORE_LBL_X)  && (x < SCORE_LBL_END_X);
    wire in_score_text  = text_y_active && (x >= SCORE_X_START) && (x < SCORE_X_END);

    wire in_hud_text    = in_ammo_label  || in_ammo_digits
                       || in_wave_label  || in_wave_text
                       || in_art_label   || in_art_digit
                       || in_gas_label   || in_gas_digit
                       || in_score_label || in_score_text;
    wire in_hud_gap     = in_hud_strip && !in_hp_bar && !in_hud_text;

    // HP bar fill: hp*2 px clamped to 192 (hp in 0..100, bar width 192).
    wire [8:0] hp_x2          = {1'b0, player_stats[7:0]} << 1;
    wire [8:0] hp_bar_width   = (hp_x2 > 9'd192) ? 9'd192 : hp_x2;
    wire       hp_filled      = ({1'b0, x[8:0]} < hp_bar_width);

    // ----- Label tile-id lookup -----
    // For each label region, compute the char index within the label (0..N-1)
    // from (x - label_start) >> 3, then map to the letter tile via case.
    // Letter tile mapping: A=16, B=17, ..., Z=41 (LETTER_TILE_BASE + L-'A').

    wire [9:0] x_minus_ammo_lbl  = x - 10'(AMMO_LBL_X);
    wire [9:0] x_minus_wave_lbl  = x - 10'(WAVE_LBL_X);
    wire [9:0] x_minus_art_lbl   = x - 10'(ART_LBL_X);
    wire [9:0] x_minus_gas_lbl   = x - 10'(GAS_LBL_X);
    wire [9:0] x_minus_score_lbl = x - 10'(SCORE_LBL_X);

    wire [1:0] ammo_char_idx  = x_minus_ammo_lbl[4:3];     // 4 chars: 0..3
    wire [1:0] wave_char_idx  = x_minus_wave_lbl[4:3];     // 4 chars: 0..3
    wire [1:0] art_char_idx   = x_minus_art_lbl[4:3];      // 3 chars: 0..2
    wire [1:0] gas_char_idx   = x_minus_gas_lbl[4:3];      // 3 chars: 0..2
    wire [2:0] score_char_idx = x_minus_score_lbl[5:3];    // 5 chars: 0..4

    logic [5:0] ammo_label_tile;
    logic [5:0] wave_label_tile;
    logic [5:0] art_label_tile;
    logic [5:0] gas_label_tile;
    logic [5:0] score_label_tile;

    // "AMMO" -> A M M O -> tiles 16, 28, 28, 30
    always_comb begin
        case (ammo_char_idx)
            2'd0:    ammo_label_tile = 6'd16;
            2'd1:    ammo_label_tile = 6'd28;
            2'd2:    ammo_label_tile = 6'd28;
            2'd3:    ammo_label_tile = 6'd30;
            default: ammo_label_tile = 6'd0;
        endcase
    end

    // "WAVE" -> W A V E -> tiles 38, 16, 37, 20
    always_comb begin
        case (wave_char_idx)
            2'd0:    wave_label_tile = 6'd38;
            2'd1:    wave_label_tile = 6'd16;
            2'd2:    wave_label_tile = 6'd37;
            2'd3:    wave_label_tile = 6'd20;
            default: wave_label_tile = 6'd0;
        endcase
    end

    // "ART" -> A R T -> tiles 16, 33, 35
    always_comb begin
        case (art_char_idx)
            2'd0:    art_label_tile = 6'd16;
            2'd1:    art_label_tile = 6'd33;
            2'd2:    art_label_tile = 6'd35;
            default: art_label_tile = 6'd0;
        endcase
    end

    // "GAS" -> G A S -> tiles 22, 16, 34
    always_comb begin
        case (gas_char_idx)
            2'd0:    gas_label_tile = 6'd22;
            2'd1:    gas_label_tile = 6'd16;
            2'd2:    gas_label_tile = 6'd34;
            default: gas_label_tile = 6'd0;
        endcase
    end

    // "SCORE" -> S C O R E -> tiles 34, 18, 30, 33, 20
    always_comb begin
        case (score_char_idx)
            3'd0:    score_label_tile = 6'd34;
            3'd1:    score_label_tile = 6'd18;
            3'd2:    score_label_tile = 6'd30;
            3'd3:    score_label_tile = 6'd33;
            3'd4:    score_label_tile = 6'd20;
            default: score_label_tile = 6'd0;
        endcase
    end

    // ----- Digit BCD selection -----
    // AMMO digits (2 chars): leftmost = tens (hud_aux[7:4]), rightmost = ones.
    wire [9:0] x_minus_ammo_dig = x - 10'(AMMO_DIG_X);
    wire       ammo_d_idx       = x_minus_ammo_dig[3];   // 0 (tens) or 1 (ones)
    wire [3:0] ammo_digit_bcd   = ammo_d_idx ? hud_aux[3:0] : hud_aux[7:4];

    // WAVE digit (2 chars): bits [15:8] of player_stats hold 2 BCD digits.
    wire [9:0] x_minus_wave_dig = x - 10'(WAVE_X_START);
    wire       wave_d_idx     = x_minus_wave_dig[3];
    wire [3:0] wave_digit_bcd = wave_d_idx ? player_stats[11:8]
                                            : player_stats[15:12];

    // ART / GAS digits: single 4-bit BCD each.
    wire [3:0] art_digit_bcd = hud_aux[11:8];
    wire [3:0] gas_digit_bcd = hud_aux[15:12];

    // SCORE digits (6 chars): score_reg[23:0] holds 6 BCD nibbles (nibble 0
    // = ones, nibble 5 = highest digit; leftmost screen column shows highest).
    wire [9:0] x_minus_score_dig  = x - 10'(SCORE_X_START);
    wire [2:0] score_digit_idx    = x_minus_score_dig[5:3];   // 0..5
    wire [2:0] score_nibble_idx   = 3'd5 - score_digit_idx;
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

    // ----- Final hud_tile_id selection -----
    // Regions are mutually exclusive by x-range, so this priority chain just
    // picks whichever region the current pixel lies in.
    logic [5:0] hud_tile_id;
    always_comb begin
        hud_tile_id = 6'd0;
        if      (in_ammo_label)  hud_tile_id = ammo_label_tile;
        else if (in_ammo_digits) hud_tile_id = 6'd48 + {2'b00, ammo_digit_bcd};
        else if (in_wave_label)  hud_tile_id = wave_label_tile;
        else if (in_wave_text)   hud_tile_id = 6'd48 + {2'b00, wave_digit_bcd};
        else if (in_art_label)   hud_tile_id = art_label_tile;
        else if (in_art_digit)   hud_tile_id = 6'd48 + {2'b00, art_digit_bcd};
        else if (in_gas_label)   hud_tile_id = gas_label_tile;
        else if (in_gas_digit)   hud_tile_id = 6'd48 + {2'b00, gas_digit_bcd};
        else if (in_score_label) hud_tile_id = score_label_tile;
        else if (in_score_text)  hud_tile_id = 6'd48 + {2'b00, score_digit_bcd};
    end

    // Tile-local pixel coordinates. All HUD regions start on 8-px x-boundaries
    // so x[2:0] gives the column within the glyph. For y, we subtract
    // TEXT_Y_START so row 0 of each glyph corresponds to y=TEXT_Y_START.
    wire [2:0] hud_tile_x    = x[2:0];
    wire [3:0] hud_tile_y_4b = y[3:0] - 4'(TEXT_Y_START);
    wire [2:0] hud_tile_y    = hud_tile_y_4b[2:0];

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
            // Glyph tile uses palette index 0xFF for lit pixels, 0x00 for blank.
            rgb_out <= (tilerom_rdata != 8'd0) ? RGB_HUD_FG : RGB_HUD_BG;
        end else if (s3_in_hud_gap) begin
            rgb_out <= RGB_HUD_BG;
        end else begin
            rgb_out <= palette_rdata;
        end
    end

endmodule
