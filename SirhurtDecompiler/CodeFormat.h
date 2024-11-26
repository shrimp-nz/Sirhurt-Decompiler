#pragma once
#include "Parser.h"

#include <ostream>

namespace Luau
{
	void formatAst(std::ostream& buff, Parser::AstStat* root);
	void formatCode(std::ostream& buff, const std::string& source);
}
