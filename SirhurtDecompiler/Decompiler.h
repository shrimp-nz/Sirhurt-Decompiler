#pragma once
#include "ByteStream.h"

#include <ostream>
#include <vector>

namespace Luau
{
	void decompile(std::ostream& buff, const std::vector<byte>& bytecode);
}
