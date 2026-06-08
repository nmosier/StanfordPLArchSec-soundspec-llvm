#pragma once

namespace llvm {

class MachineInstr;

namespace X86::soundspec {

bool maybeSpeculationPrimitive(const MachineInstr &MI);

}

}
