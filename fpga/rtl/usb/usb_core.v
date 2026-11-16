`default_nettype none

module usb_core (
    input  wire        clk48,
    input  wire        clk,
    input  wire        rst,
    input  wire        dp_in,
    input  wire        dn_in,
    output wire        dp_out,
    output wire        dn_out,
    output wire        oe,
    output wire        pu_en,
    input  wire        wr,
    input  wire [15:0] addr,
    input  wire [7:0]  wdata,
    output wire [7:0]  rdata,
    output wire        hit,
    input  wire        b1_wr,
    input  wire [11:0] b1_addr,
    output wire [7:0]  b1_rdata,
    output wire        irq_pulse,
    output wire        irq_level
);
    wire       ev_tog, se0_long;
    wire [1:0] ev_kind, line;
    wire [7:0] ev_data;
    wire [2:0] ev_flags;
    usb_phy_rx u_rx (
        .clk(clk48), .dp(dp_in), .dn(dn_in), .en(!oe),
        .ev_tog(ev_tog), .ev_kind(ev_kind), .ev_data(ev_data), .ev_flags(ev_flags),
        .se0_long(se0_long), .line(line)
    );

    wire       b_tog, b_last, b_crc, b_taken, tx_done;
    wire [7:0] b_data;
    usb_phy_tx u_tx (
        .clk(clk48), .b_tog(b_tog), .b_data(b_data), .b_last(b_last), .b_crc(b_crc),
        .b_taken(b_taken), .done_tog(tx_done), .oe(oe), .dp(dp_out), .dn(dn_out)
    );

    usb_sie u_sie (
        .clk(clk), .rst(rst),
        .ev_tog(ev_tog), .ev_kind(ev_kind), .ev_data(ev_data), .ev_flags(ev_flags),
        .se0_long(se0_long), .line(line),
        .b_tog(b_tog), .b_data(b_data), .b_last(b_last), .b_crc(b_crc),
        .b_taken(b_taken), .tx_done(tx_done), .pu_en(pu_en),
        .wr(wr), .addr(addr), .wdata(wdata), .rdata(rdata), .hit(hit),
        .b1_wr(b1_wr), .b1_addr(b1_addr), .b1_rdata(b1_rdata),
        .irq_pulse(irq_pulse), .irq_level(irq_level)
    );
endmodule

`default_nettype wire
