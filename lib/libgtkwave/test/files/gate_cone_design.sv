// Small hierarchical design for exercising the gate-level cone window
// (src/gate_schematic_view.c): a registered result fed by an ALU, which is
// fed by a register file, so the fan-in cone of `result` is several hops
// deep and crosses two sub-module boundaries.
module alu(input [3:0] a, input [3:0] b, input [1:0] op, output reg [3:0] y);
  always @* begin
    case (op)
      2'd0: y = a + b;
      2'd1: y = a & b;
      2'd2: y = a | b;
      default: y = a ^ b;
    endcase
  end
endmodule

module regfile(input clk, input we, input [1:0] wa, input [1:0] ra,
               input [3:0] wd, output [3:0] rd);
  reg [3:0] r0, r1, r2, r3;
  always @(posedge clk) begin
    if (we) begin
      case (wa)
        2'd0: r0 <= wd;
        2'd1: r1 <= wd;
        2'd2: r2 <= wd;
        default: r3 <= wd;
      endcase
    end
  end
  assign rd = (ra == 2'd0) ? r0 : (ra == 2'd1) ? r1 : (ra == 2'd2) ? r2 : r3;
endmodule

module cpu_top(input clk, input [1:0] op, input [3:0] din, input we,
               input [1:0] wa, input [1:0] ra, output [3:0] result);
  wire [3:0] rd;
  wire [3:0] alu_y;
  reg [3:0] acc;
  regfile u_rf(.clk(clk), .we(we), .wa(wa), .ra(ra), .wd(acc), .rd(rd));
  alu u_alu(.a(rd), .b(din), .op(op), .y(alu_y));
  always @(posedge clk) acc <= alu_y;
  assign result = acc ^ din;
endmodule
