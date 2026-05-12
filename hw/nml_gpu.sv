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

    // Invert so the ADV7123 DAC samples R/G/B mid-cycle (on its rising edge,
    // which is now pix_clk's falling edge), well after the FPGA has updated
    // them on pix_clk's rising edge. Avoids hold-time violations at the DAC.
    assign vga_clk = ~pix_clk;

    // =========================================================================
    // 2. INTERNAL WIRES
    // =========================================================================
    // VGA Timing
    logic [9:0] x, y;
    logic visible, hsync, vsync, vblank, hblank;

    // Avalon / Register File
    logic ctrl_enable, ctrl_swap_req, ctrl_hud_on;
    logic [31:0] bg_scroll, player_pos, player_stats, score, kill_count;
    logic [31:0] hud_aux;
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

    // Cross ctrl_swap_req from clk (50 MHz) into pix_clk (25 MHz). The Avalon
    // side now holds the request for several clk cycles, so a 2-FF synchroniser
    // here will reliably catch it; we then edge-detect to set swap_pending
    // exactly once per request rather than continuously while the source is
    // held high.
    logic ctrl_swap_req_sync_a, ctrl_swap_req_sync_b, ctrl_swap_req_sync_c;
    always_ff @(posedge pix_clk or negedge reset_n) begin
        if (!reset_n) begin
            ctrl_swap_req_sync_a <= 1'b0;
            ctrl_swap_req_sync_b <= 1'b0;
            ctrl_swap_req_sync_c <= 1'b0;
        end else begin
            ctrl_swap_req_sync_a <= ctrl_swap_req;
            ctrl_swap_req_sync_b <= ctrl_swap_req_sync_a;
            ctrl_swap_req_sync_c <= ctrl_swap_req_sync_b;
        end
    end
    wire ctrl_swap_req_pulse = ctrl_swap_req_sync_b && !ctrl_swap_req_sync_c;

    // Swap shadow to active on VSYNC if requested
    always_ff @(posedge pix_clk or negedge reset_n) begin
        if (!reset_n) swap_pending <= 1'b0;
        else begin
            if (ctrl_swap_req_pulse) swap_pending <= 1'b1;
            if (swap_pending && vsync) begin
                for (int i=0; i<32; i++) sprite_table_active[i] <= sprite_table_shadow[i];
                swap_pending <= 1'b0;
            end
        end
    end

    // Connect sprite_eval to the active sprite table. sprite_table_active is
    // written in this same pix_clk domain on the swap pulse above, so a
    // combinational read here is in-domain. Drives the 32-to-1 mux that
    // feeds sprite_eval's FETCH/EVAL pipeline.
    always_comb eval_sprtab_rdata = sprite_table_active[eval_sprtab_raddr];

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

    // C. Tile Map RAM -- stored as 32-bit words (1200 x 4 B = 4800 B).
    // Background: the HPS-LW bridge implements ARM STRB (byte writes) as a
    // {word-read, byte-modify, word-write} sequence at the slave with
    // byteenable=4'b1111. With byte-organized storage we could only return
    // one byte per word read; the bridge then saw {0,0,0,byte0} on readback
    // and wrote {0,0,0,byte0}+mod back, silently dropping bytes 1/2/3 at
    // every word. The visible symptom was "only every 4th column gets the
    // new tile; the other 3 stay at the reset value 0 (brown)".
    //
    // 32-bit-word storage lets us (a) return all 4 bytes per word read so
    // bridge RMW preserves untouched bytes, and (b) gate per-lane writes on
    // avs_byteenable so any sub-word or full-word write commits exactly the
    // intended bytes. Quartus infers this as an M10K with native byteena_a.
    logic [31:0] tilemap_ram [0:1199];
    logic [12:0] tilemap_raddr;
    logic [7:0]  tilemap_rdata;
    logic [31:0] tilemap_rdata_sw;
    logic [31:0] tilemap_word_pix;
    logic [1:0]  tilemap_raddr_lo_r;

    always_ff @(posedge clk) begin
        if (tilemap_we) begin
            if (avs_byteenable[0]) tilemap_ram[mem_waddr[12:2]][7:0]   <= mem_wdata[7:0];
            if (avs_byteenable[1]) tilemap_ram[mem_waddr[12:2]][15:8]  <= mem_wdata[15:8];
            if (avs_byteenable[2]) tilemap_ram[mem_waddr[12:2]][23:16] <= mem_wdata[23:16];
            if (avs_byteenable[3]) tilemap_ram[mem_waddr[12:2]][31:24] <= mem_wdata[31:24];
        end
        tilemap_rdata_sw <= tilemap_ram[mem_waddr[12:2]];
    end

    // Pix-clk read port: register the word, then combinationally extract the
    // byte the compositor asked for. Same 1-cycle raddr->rdata latency as
    // before, so compositor pipeline stays unchanged.
    always_ff @(posedge pix_clk) begin
        tilemap_word_pix   <= tilemap_ram[tilemap_raddr[12:2]];
        tilemap_raddr_lo_r <= tilemap_raddr[1:0];
    end
    always_comb begin
        case (tilemap_raddr_lo_r)
            2'b00:   tilemap_rdata = tilemap_word_pix[7:0];
            2'b01:   tilemap_rdata = tilemap_word_pix[15:8];
            2'b10:   tilemap_rdata = tilemap_word_pix[23:16];
            2'b11:   tilemap_rdata = tilemap_word_pix[31:24];
            default: tilemap_rdata = 8'd0;
        endcase
    end

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
        .avs_readdata(avs_readdata), .avs_waitrequest(avs_waitrequest),
        .vblank_in(vblank), .swap_pending_in(swap_pending), .frame_ctr_in(8'd0),
        .ctrl_enable(ctrl_enable), .ctrl_swap_req(ctrl_swap_req), .ctrl_hud_on(ctrl_hud_on),
        .bg_scroll(bg_scroll),
        .sprtab_we(sprtab_we), .palette_we(palette_we), .tilemap_we(tilemap_we),
        .mem_waddr(mem_waddr), .mem_wdata(mem_wdata),
        .sprtab_rdata_sw(sprtab_rdata_sw), .palette_rdata_sw(palette_rdata_sw), .tilemap_rdata_sw(tilemap_rdata_sw),
        .player_pos(player_pos), .player_stats(player_stats), .score_reg(score), .kill_count(kill_count),
        .hud_aux(hud_aux)
    );

    vga_timing timing_inst (
        .pix_clk(pix_clk), .reset_n(reset_n),
        .x(x), .y(y),
        .visible(visible), .hsync(hsync), .vsync(vsync), .vblank(vblank), .hblank(hblank)
    );

    // Strobe eval logic slightly after HBLANK starts to ensure next line is ready
    assign eval_strobe = (x == 642);

    logic eval_done_pulse;
    sprite_eval eval_inst (
        .pix_clk(pix_clk), .reset_n(reset_n),
        .next_scanline(y + 10'd1), .eval_strobe(eval_strobe),
        .sprtab_raddr(eval_sprtab_raddr), .sprtab_rdata(eval_sprtab_rdata),
        .line_sprites(line_sprites), .active_mask(active_mask),
        .eval_done(eval_done_pulse)
    );

    // Sustain the one-cycle DONE pulse from sprite_eval until the next
    // eval_strobe, so sprite_fetch reliably observes "eval finished" no
    // matter when in CLEAR_BUF it samples the signal. The previous version
    // edge-detected on active_mask transitions, which silently failed for
    // the 15-of-16 scanlines where the same set of sprites stays active.
    logic eval_done_r;
    always_ff @(posedge pix_clk or negedge reset_n) begin
        if (!reset_n)             eval_done_r <= 1'b0;
        else if (eval_strobe)     eval_done_r <= 1'b0;
        else if (eval_done_pulse) eval_done_r <= 1'b1;
    end
    assign eval_done = eval_done_r;

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
        // HUD overlay routing
        .hud_on(ctrl_hud_on),
        .player_stats(player_stats),
        .score_reg(score),
        .hud_aux(hud_aux),
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