// UPduino v3.1 bring-up design "hello": 12 MHz -> PLL 48 MHz, status LED, UART banner + echo.
//
// LED:  green blink  = PLL locked, design running on 48 MHz
//       red blink    = PLL not locked (no 12 MHz: check R16 "OSC" is shorted)
//       blue flash   = UART byte received
// UART: 115200 8N1 on gpio_2 (TX) / gpio_46 (RX). Prints a banner at start-up and
//       whenever Enter is pressed; echoes everything else.
`default_nettype none

module top_hello (
    input  wire clk12,
    output wire led_red,
    output wire led_green,
    output wire led_blue,
    output wire flash_csn,
    output wire uart_tx,
    input  wire uart_rx
);
    assign flash_csn = 1'b1;

    // ---------------------------------------------------------------- clocks
    // 12 MHz * (DIVF+1) / (2^DIVQ * (DIVR+1)) = 12 * 64 / 16 = 48 MHz (VCO 768 MHz)
    wire clk48, pll_locked;
    SB_PLL40_CORE #(
        .FEEDBACK_PATH("SIMPLE"),
        .DIVR(4'b0000),
        .DIVF(7'b0111111),
        .DIVQ(3'b100),
        .FILTER_RANGE(3'b001)
    ) u_pll (
        .REFERENCECLK(clk12),
        .PLLOUTCORE  (clk48),
        .LOCK        (pll_locked),
        .RESETB      (1'b1),
        .BYPASS      (1'b0)
    );

    // Low-frequency oscillator (~10 kHz) so the LED can report "no clock".
    wire clk_lf;
    SB_LFOSC u_lfosc (.CLKLFPU(1'b1), .CLKLFEN(1'b1), .CLKLF(clk_lf));
    reg [12:0] lf_cnt = 0;
    always @(posedge clk_lf) lf_cnt <= lf_cnt + 1'b1;

    // ---------------------------------------------------------------- reset
    reg [1:0]  lock_sync = 0;
    reg [15:0] rst_cnt   = 0;
    wire       rst       = !rst_cnt[15];
    always @(posedge clk48) begin
        lock_sync <= {lock_sync[0], pll_locked};
        if (!lock_sync[1])   rst_cnt <= 0;
        else if (rst)        rst_cnt <= rst_cnt + 1'b1;
    end

    // ---------------------------------------------------------------- UART
    wire [7:0] rx_data;
    wire       rx_valid;
    uart_rx #(.CLKS_PER_BIT(417)) u_rx (
        .clk(clk48), .rst(rst), .rx(uart_rx), .data(rx_data), .valid(rx_valid)
    );

    reg  [7:0] tx_data;
    reg        tx_valid;
    wire       tx_ready;
    uart_tx #(.CLKS_PER_BIT(417)) u_tx (
        .clk(clk48), .rst(rst), .data(tx_data), .valid(tx_valid), .ready(tx_ready), .tx(uart_tx)
    );

    // Banner text
    localparam integer BANNER_LEN = 31;
    reg [4:0] banner_idx;
    reg       banner_active;
    reg [7:0] banner_char;
    always @(*) begin
        case (banner_idx)
            5'd0:  banner_char = 8'h0D;
            5'd1:  banner_char = "\n";
            5'd2:  banner_char = "o";
            5'd3:  banner_char = "p";
            5'd4:  banner_char = "e";
            5'd5:  banner_char = "n";
            5'd6:  banner_char = "o";
            5'd7:  banner_char = "x";
            5'd8:  banner_char = "-";
            5'd9:  banner_char = "f";
            5'd10: banner_char = "p";
            5'd11: banner_char = "g";
            5'd12: banner_char = "a";
            5'd13: banner_char = " ";
            5'd14: banner_char = "u";
            5'd15: banner_char = "p";
            5'd16: banner_char = "d";
            5'd17: banner_char = "u";
            5'd18: banner_char = "i";
            5'd19: banner_char = "n";
            5'd20: banner_char = "o";
            5'd21: banner_char = "3";
            5'd22: banner_char = "1";
            5'd23: banner_char = " ";
            5'd24: banner_char = "4";
            5'd25: banner_char = "8";
            5'd26: banner_char = "M";
            5'd27: banner_char = "H";
            5'd28: banner_char = "z";
            5'd29: banner_char = 8'h0D;
            default: banner_char = "\n";
        endcase
    end

    // One-byte holding register for echo so a byte arriving while TX is busy isn't lost.
    reg [7:0] echo_byte;
    reg       echo_pending;

    always @(posedge clk48) begin
        tx_valid <= 1'b0;
        if (rst) begin
            banner_active <= 1'b1;
            banner_idx    <= 0;
            echo_pending  <= 1'b0;
        end else begin
            if (rx_valid) begin
                if (rx_data == 8'h0D) begin
                    banner_active <= 1'b1;
                    banner_idx    <= 0;
                end else begin
                    echo_byte    <= rx_data;
                    echo_pending <= 1'b1;
                end
            end

            if (tx_ready && !tx_valid) begin
                if (banner_active) begin
                    tx_data  <= banner_char;
                    tx_valid <= 1'b1;
                    if (banner_idx == BANNER_LEN - 1) banner_active <= 1'b0;
                    banner_idx <= banner_idx + 1'b1;
                end else if (echo_pending && !rx_valid) begin
                    tx_data      <= echo_byte;
                    tx_valid     <= 1'b1;
                    echo_pending <= 1'b0;
                end
            end
        end
    end

    // ---------------------------------------------------------------- LED
    reg [24:0] blink_cnt = 0;
    reg [21:0] rx_flash  = 0;      // ~87 ms at 48 MHz
    always @(posedge clk48) begin
        blink_cnt <= blink_cnt + 1'b1;
        if (rx_valid)           rx_flash <= {22{1'b1}};
        else if (rx_flash != 0) rx_flash <= rx_flash - 1'b1;
    end

    wire on_green = pll_locked && blink_cnt[24];
    wire on_red   = !pll_locked && lf_cnt[12];
    wire on_blue  = pll_locked && (rx_flash != 0);

    SB_RGBA_DRV #(
        .CURRENT_MODE("0b1"),          // half-current mode
        .RGB0_CURRENT("0b000001"),     // ~2 mA: plenty for an indicator
        .RGB1_CURRENT("0b000001"),
        .RGB2_CURRENT("0b000001")
    ) u_rgb (
        .CURREN  (1'b1),
        .RGBLEDEN(1'b1),
        .RGB0PWM (on_green),
        .RGB1PWM (on_blue),
        .RGB2PWM (on_red),
        .RGB0    (led_green),
        .RGB1    (led_blue),
        .RGB2    (led_red)
    );
endmodule

`default_nettype wire
