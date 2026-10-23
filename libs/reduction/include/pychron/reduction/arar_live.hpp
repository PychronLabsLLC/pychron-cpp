// What the live Ar-Ar path (arar.hpp, detail/arar_kernels.hpp) shares with the
// spec'd Ar-Ar reduction (libs/arar arar_types.hpp, which includes this). Kept
// in libs/reduction so the live path depends on nothing in libs/arar.
#pragma once

namespace pychron::reduction {

enum class K3739Mode { Normal, Fixed };

}  // namespace pychron::reduction
