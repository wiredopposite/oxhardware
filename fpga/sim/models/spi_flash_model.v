// Behavioural SPI NOR model (W25Q-class subset) for simulation: mode 0, commands
// 0xAB (release power-down, ignored) and 0x03 (READ, 24-bit address, sequential).
// Contents: 1 MB, initialised to FF; tests load images into 'mem'.
`timescale 1ns/1ps
module spi_flash_model #(parameter real T_V = 7.0) (
    input  wire cs_n,
    input  wire sck,
    input  wire mosi,
    output reg  miso = 1'bz
);
    reg [7:0]  mem [0:20'hFFFFF];
    reg [31:0] sh;
    integer    nbits;
    reg        reading;
    reg [23:0] addr;
    integer    bitn;
    integer    i;
    initial for (i = 0; i <= 20'hFFFFF; i = i + 1) mem[i] = 8'hFF;

    always @(negedge cs_n) begin nbits = 0; reading = 0; end
    always @(posedge cs_n) begin miso <= #(T_V) 1'bz; reading = 0; end

    always @(posedge sck) if (!cs_n && !reading) begin
        sh = {sh[30:0], mosi};
        nbits = nbits + 1;
        if (nbits == 32 && sh[31:24] == 8'h03) begin
            reading = 1;
            addr    = sh[23:0];
            bitn    = 7;
        end
    end

    always @(negedge sck) if (!cs_n && reading) begin
        miso <= #(T_V) mem[addr[19:0]][bitn];
        if (bitn == 0) begin bitn = 7; addr = addr + 1; end
        else bitn = bitn - 1;
    end
endmodule
