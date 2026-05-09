`timescale 1ns / 1ps

module nml_gpu (
    // Avalon-MM slave (Lightweight bridge, 50 MHz)
    input  logic        clk,
    input  logic        reset_n,
    input  logic [13:0] avs_address,
    input  logic        avs_read,
    input  logic        avs_write,
    input  logic [31:0] avs_writedata,
    input  logic [3:0]  avs_byteenable,
    
    output logic [31:0] avs_readdata,
    output logic        avs_waitrequest, // Tied 0; single-cycle

    // VGA DAC Output
    output logic [7:0]  vga_r, vga_g, vga_b,
    output logic        vga_hs, vga_vs,
    output logic        vga_blank_n, vga_sync_n, vga_clk
);

    assign avs_waitrequest = 1'b0;
    assign vga_sync_n = 1'b0; // Not used for our DAC

    // =========================================================================
    // 1. CLOCK GENERATION (50 MHz -> 25 MHz pixel clock)
    // =========================================================================
    logic pix_clk;

    pll_25mhz pll_inst (
        .refclk  (clk),
        .rst     (~reset_n),
        .outclk_0(pix_clk)
    );

    assign vga_clk = pix_clk;

    // =========================================================================
    // 2. INTERNAL WIRES
    // =========================================================================
    // VGA Timing
    logic [9:0] x, y;
    logic visible, hsync, vsync, vblank, hblank;

    // Avalon / Register File
    logic ctrl_enable, ctrl_swap_req, ctrl_hud_on;
    logic [31:0] bg_scroll, player_pos, player_stats, score, kill_count;
    logic [12:0] mem_waddr;
    logic sprtab_we, palette_we, tilemap_we;
    logic [31:0] mem_wdata;
    
    // Status tracking
    logic swap_pending;

    // Sprite Eval -> Sprite Fetch
    logic [4:0]  eval_sprtab_raddr;
    logic [63:0] eval_sprtab_rdata;
    logic [63:0] line_sprites [0:7];
    logic [7:0]  active_mask;
    logic        eval_strobe;
    logic        eval_done;

    // Line Buffer Control
    logic linebuf_sel; // 0 = A fills/B drains, 1 = B fills/A drains

    // =========================================================================
    // 3. MEMORIES (Inferred M10K RAMs)
    // =========================================================================
    // A. Sprite Table (Double Buffered internally for tear-free rendering)
    logic [63:0] sprite_table_active [0:31];
    logic [63:0] sprite_table_shadow [0:31];
    logic [31:0] sprtab_rdata_sw;
    
    always_ff @(posedge clk) begin
        // Avalon Write to Shadow
        if (sprtab_we) begin
            // 32-bit writes into 64-bit memory slots
            if (mem_waddr[0] == 1'b0) sprite_table_shadow[mem_waddr[5:1]][31:0]  <= mem_wdata;
            else                      sprite_table_shadow[mem_waddr[5:1]][63:32] <= mem_wdata;
        end
        // Avalon Read from Shadow
        sprtab_rdata_sw <= (mem_waddr[0] == 1'b0) ? sprite_table_shadow[mem_waddr[5:1]][31:0] : 
                                                    sprite_table_shadow[mem_waddr[5:1]][63:32];
    end

    // Swap shadow to active on VSYNC if requested
    always_ff @(posedge pix_clk or negedge reset_n) begin
        if (!reset_n) swap_pending <= 1'b0;
        else begin
            if (ctrl_swap_req) swap_pending <= 1'b1;
            if (swap_pending && vsync) begin
                for (int i=0; i<32; i++) sprite_table_active[i] <= sprite_table_shadow[i];
                swap_pending <= 1'b0;
            end
        end
    end

    assign eval_sprtab_rdata = sprite_table_active[eval_sprtab_raddr];

    // B. Palette RAM
    logic [23:0] palette_ram [0:255];
    logic [7:0]  palette_raddr;
    logic [23:0] palette_rdata;
    logic [31:0] palette_rdata_sw;

    always_ff @(posedge clk) begin
        if (palette_we) palette_ram[mem_waddr[7:0]] <= mem_wdata[23:0];
        palette_rdata_sw <= {8'd0, palette_ram[mem_waddr[7:0]]};
    end
    always_ff @(posedge pix_clk) palette_rdata <= palette_ram[palette_raddr];

    // C. Tile Map RAM
    logic [7:0] tilemap_ram [0:4799];
    logic [12:0] tilemap_raddr;
    logic [7:0]  tilemap_rdata;
    logic [31:0] tilemap_rdata_sw;

    always_ff @(posedge clk) begin
        if (tilemap_we) tilemap_ram[mem_waddr] <= mem_wdata[7:0];
        tilemap_rdata_sw <= {24'd0, tilemap_ram[mem_waddr]};
    end
    always_ff @(posedge pix_clk) tilemap_rdata <= tilemap_ram[tilemap_raddr];

    // D. Sprite ROM & Tile ROM ($readmemh initialization)
    logic [7:0] sprite_rom [0:16383];
    logic [7:0] tile_rom   [0:4095];
    logic [13:0] sprrom_raddr;
    logic [7:0]  sprrom_rdata;
    logic [11:0] tilerom_raddr;
    logic [7:0]  tilerom_rdata;

    initial begin
        $readmemh("sprite_rom.hex",    sprite_rom);
        $readmemh("tile_rom.hex",      tile_rom);
        // Smoke-test pre-init: palette + initial sprite table.
        // Once the C driver on the HPS writes these regions, runtime values
        // override the init data on the next SWAP.
        $readmemh("palette.hex",       palette_ram);
        $readmemh("sprite_table.hex",  sprite_table_active);
        $readmemh("sprite_table.hex",  sprite_table_shadow);
    end

    always_ff @(posedge pix_clk) sprrom_rdata <= sprite_rom[sprrom_raddr];
    always_ff @(posedge pix_clk) tilerom_rdata <= tile_rom[tilerom_raddr];

    // E. Double Line Buffers (Ping-Pong)
    logic [7:0] linebuf_A [0:639];
    logic [7:0] linebuf_B [0:639];
    
    logic [9:0] fetch_waddr;
    logic [7:0] fetch_wdata;
    logic       fetch_we;
    logic [9:0] comp_raddr;
    logic [7:0] comp_rdata;

    always_ff @(posedge pix_clk or negedge reset_n) begin
        if (!reset_n) linebuf_sel <= 1'b0;
        else if (hblank && x == 640) linebuf_sel <= ~linebuf_sel; // Toggle at end of visible line
    end

    // Ping-Pong Routing
    always_ff @(posedge pix_clk) begin
        // Buffer A
        if (linebuf_sel == 1'b0 && fetch_we) linebuf_A[fetch_waddr] <= fetch_wdata;
        // Buffer B
        if (linebuf_sel == 1'b1 && fetch_we) linebuf_B[fetch_waddr] <= fetch_wdata;
    end
    
    // Compositor reads from the opposite buffer
    assign comp_rdata = (linebuf_sel == 1'b1) ? linebuf_A[comp_raddr] : linebuf_B[comp_raddr];

    // =========================================================================
    // 4. MODULE INSTANTIATIONS
    // =========================================================================
    avalon_slave_iface avalon_inst (
        .clk(clk), .reset_n(reset_n),
        .avs_address(avs_address), .avs_read(avs_read), .avs_write(avs_write),
        .avs_writedata(avs_writedata), .avs_byteenable(avs_byteenable),
        .avs_readdata(avs_readdata),
        .vblank_in(vblank), .swap_pending_in(swap_pending), .frame_ctr_in(8'd0),
        .ctrl_enable(ctrl_enable), .ctrl_swap_req(ctrl_swap_req), .ctrl_hud_on(ctrl_hud_on),
        .bg_scroll(bg_scroll),
        .sprtab_we(sprtab_we), .palette_we(palette_we), .tilemap_we(tilemap_we),
        .mem_waddr(mem_waddr), .mem_wdata(mem_wdata),
        .sprtab_rdata_sw(sprtab_rdata_sw), .palette_rdata_sw(palette_rdata_sw), .tilemap_rdata_sw(tilemap_rdata_sw),
        .player_pos(player_pos), .player_stats(player_stats), .score_reg(score), .kill_count(kill_count)
    );

    vga_timing timing_inst (
        .pix_clk(pix_clk), .reset_n(reset_n),
        .x(x), .y(y),
        .visible(visible), .hsync(hsync), .vsync(vsync), .vblank(vblank), .hblank(hblank)
    );

    // Strobe eval logic slightly after HBLANK starts to ensure next line is ready
    assign eval_strobe = (x == 642);

    sprite_eval eval_inst (
        .pix_clk(pix_clk), .reset_n(reset_n),
        .next_scanline(y + 10'd1), .eval_strobe(eval_strobe),
        .sprtab_raddr(eval_sprtab_raddr), .sprtab_rdata(eval_sprtab_rdata),
        .line_sprites(line_sprites), .active_mask(active_mask)
    );
    
    // Simple edge detector to start sprite fetch when eval finishes
    logic [7:0] active_mask_d;
    always_ff @(posedge pix_clk) active_mask_d <= active_mask;
    assign eval_done = (active_mask != active_mask_d);

    sprite_fetch fetch_inst (
        .pix_clk(pix_clk), .reset_n(reset_n),
        .hblank(hblank), .next_scanline(y + 10'd1),
        .eval_done(eval_done), .active_mask(active_mask), .line_sprites(line_sprites),
        .sprrom_raddr(sprrom_raddr), .sprrom_rdata(sprrom_rdata),
        .linebuf_waddr(fetch_waddr), .linebuf_wdata(fetch_wdata), .linebuf_we(fetch_we)
    );

    logic [23:0] rgb_out;
    compositor comp_inst (
        .pix_clk(pix_clk), .reset_n(reset_n),
        .x(x), .y(y), .visible(visible),
        .scroll_x(bg_scroll[15:0]), .scroll_y(bg_scroll[31:16]),
        .tilemap_raddr(tilemap_raddr), .tilemap_rdata(tilemap_rdata),
        .tilerom_raddr(tilerom_raddr), .tilerom_rdata(tilerom_rdata),
        .linebuf_raddr(comp_raddr), .linebuf_rdata(comp_rdata),
        .palette_raddr(palette_raddr), .palette_rdata(palette_rdata),
        .rgb_out(rgb_out)
    );

    // Final Output Routing
    assign vga_r = ctrl_enable ? rgb_out[23:16] : 8'd0;
    assign vga_g = ctrl_enable ? rgb_out[15:8]  : 8'd0;
    assign vga_b = ctrl_enable ? rgb_out[7:0]   : 8'd0;
    assign vga_hs = hsync;
    assign vga_vs = vsync;
    assign vga_blank_n = visible;

endmodule