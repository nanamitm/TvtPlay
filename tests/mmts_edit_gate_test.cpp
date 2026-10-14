#include "../src/MmtsEditGate.h"
#include <cassert>
#include <iostream>
int main() {
    MmtsEditGate gate;
    gate.SetRange(100, 200);
    assert(gate.Accept(1, 150, true, false));
    assert(!gate.Accept(2, 50, true, false));
    assert(gate.Accept(1, -1, false, false));
    assert(!gate.Accept(2, -1, false, true));
    assert(gate.Accept(1, -1, false, true));
    assert(!gate.Accept(1, -1, false, true)); // orphan after completion
    assert(!gate.Accept(1, 200, true, false));
    assert(!gate.Accept(1, -1, false, true));
    assert(gate.Accept(1, 100, true, false));
    gate.Drop(1);
    assert(!gate.Accept(1, -1, false, true));
    assert(gate.Accept(1, 120, true, false));
    gate.SetRange(300, 400);
    assert(!gate.Accept(1, -1, false, true));
    assert(gate.Accept(1, 350, true, true));
    std::cout << "MMTS edit fragment gate tests passed\n";
}
