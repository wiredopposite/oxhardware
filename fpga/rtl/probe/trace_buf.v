// In-FPGA logic analyser: records a 16-bit sample every clock of 'sclk' into a 512-entry
// circular buffer (2 EBR). On 'trigger' (first one after arm) it records POST more samples
// and freezes. The buffer is read from another clock domain ('rclk') once frozen.
`default_nettype none

module trace_buf #(
    parameter integer POST = 64
) (
    input  wire        sclk,
    input  wire [15:0] sample,
    input  wire        trigger,
    output reg         frozen = 1'b0,
    output reg  [8:0]  wp = 0,             // next write slot = oldest sample once frozen
    // read port
    input  wire        rclk,
    input  wire [8:0]  raddr,
    output reg  [15:0] rdata
);
    reg [15:0] mem [0:511];
    reg        armed = 1'b1, trig = 1'b0;
    reg [7:0]  post = 0;

    always @(posedge sclk) begin
        if (!frozen) begin
            mem[wp] <= sample;
            wp      <= wp + 1'b1;
            if (trig) begin
                if (post == POST - 1) frozen <= 1'b1;
                post <= post + 1'b1;
            end else if (trigger && armed) begin
                trig  <= 1'b1;
                armed <= 1'b0;
            end
        end
    end

    always @(posedge rclk) rdata <= mem[raddr];
endmodule

`default_nettype wire
