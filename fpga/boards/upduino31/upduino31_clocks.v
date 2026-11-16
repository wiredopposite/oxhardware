`default_nettype none

module upduino31_clocks #(
    parameter [2:0] DIVQ = 3'b100,  // 4: 48MHz, 5: 24MHz
    parameter       DUAL = 0
) (
    input  wire clk12,
    output wire clk_sys,
    output wire clk_2x,
    output wire pll_locked,
    output wire rst,
    output wire clk_lf
);
    // The two-output PLL can't be used: its second output takes pin 37, LCLK.
    wire pll_out;
    SB_PLL40_CORE #(
        .FEEDBACK_PATH("SIMPLE"),
        .DIVR(4'b0000),
        .DIVF(7'b0111111),
        .DIVQ(DUAL ? 3'b100 : DIVQ),
        .FILTER_RANGE(3'b001)
    ) u_pll (
        .REFERENCECLK(clk12),
        .PLLOUTGLOBAL(pll_out),
        .LOCK        (pll_locked),
        .RESETB      (1'b1),
        .BYPASS      (1'b0)
    );
    generate if (DUAL) begin : g_dual
        reg half = 1'b0;
        always @(posedge pll_out) half <= ~half;
        SB_GB u_gb (.USER_SIGNAL_TO_GLOBAL_BUFFER(half), .GLOBAL_BUFFER_OUTPUT(clk_sys));
        assign clk_2x = pll_out;
    end else begin : g_single
        assign clk_sys = pll_out;
        assign clk_2x  = 1'b0;
    end endgenerate

    SB_LFOSC u_lfosc (.CLKLFPU(1'b1), .CLKLFEN(1'b1), .CLKLF(clk_lf));

    reg [1:0]  lock_sync = 0;
    reg [15:0] rst_cnt   = 0;
    assign rst = !rst_cnt[15];
    always @(posedge clk_sys) begin
        lock_sync <= {lock_sync[0], pll_locked};
        if (!lock_sync[1]) rst_cnt <= 0;
        else if (rst)      rst_cnt <= rst_cnt + 1'b1;
    end
endmodule

`default_nettype wire
