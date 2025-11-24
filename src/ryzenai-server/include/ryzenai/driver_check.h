#pragma once

namespace ryzenai {

// Checks the NPU driver version and warns if it's too old
// Returns true if the driver check passes, false otherwise
bool CheckNPUDriverVersion();

}

