// UPduino v3.1 clocking: 12 MHz (pin 20, R16 "OSC" shorted) -> PLL 48 MHz (DIVQ=4)
// or 24 MHz (DIVQ=5), synchronous reset released 2^15 clocks after PLL lock, and the ~10 kHz LFOSC
// (runs even without the 12 MHz clock, for "no clock" indication).
`default_nettype none

module upduino31_clocks #(
    parameter [2:0] DIVQ = 3'b100   // 4: 48 MHz, 5: 24 MHz
) (
    input  wire clk12,
    output wire clk_sys,
    output wire pll_locked,
    output wire rst,          // sync to clk_sys, active high
    output wire clk_lf
);
    // 12 MHz * (DIVF+1) / (2^DIVQ * (DIVR+1)) = 768 MHz VCO / 2^DIVQ
    SB_PLL40_CORE #(
        .FEEDBACK_PATH("SIMPLE"),
        .DIVR(4'b0000),
        .DIVF(7'b0111111),
        .DIVQ(DIVQ),
        .FILTER_RANGE(3'b001)
    ) u_pll (
        .REFERENCECLK(clk12),
        .PLLOUTCORE  (clk_sys),
        .LOCK        (pll_locked),
        .RESETB      (1'b1),
        .BYPASS      (1'b0)
    );

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
