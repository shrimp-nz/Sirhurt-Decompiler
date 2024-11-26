#include "CodeFormat.h"
#include "Parser.h"

#include <sstream>

using namespace Luau;

class CodeVisitor : public Parser::AstVisitor
{
	std::ostream& buff;
	uint32_t indent = 0;
	bool mainEncountered = false;

	void writeIndent()
	{
		std::fill_n(std::ostream_iterator<char>(buff), indent * 4, ' ');
	}

	enum class StringQuoteType
	{
		Long,
		Single,
		Double,
		Escape
	};
	static StringQuoteType getStringQuoteType(const std::string& str)
	{
		bool hasSingle = false, hasDouble = false;
		for (size_t i = 0; i < str.size(); ++i)
		{
			char c = str[i];

			// TODO: nested long strINNNGSS
			if (c == '\n' || c == '\\')
			{
				return StringQuoteType::Long;
			}

			if (c == '"')
			{
				hasDouble = true;
			} else if (c == '\'')
			{
				hasSingle = true;
			}
		}

		return hasSingle && !hasDouble || !hasSingle && !hasDouble ? StringQuoteType::Double :
			hasDouble && !hasSingle ? StringQuoteType::Single : StringQuoteType::Escape;
	}

	static bool isValidName(const std::string& str)
	{
		for (size_t i = 0; i < str.size(); ++i)
		{
			char c = str[i];
			if (i == 0)
			{
				if (isalpha(c) || c == '_')
				{
					continue;
				}

				return false;
			}

			if (isalnum(c) || c == '_')
			{
				continue;
			}

			return false;
		}

		return true;
	}

	void visitIf(Parser::AstStatIf* ifStat)
	{
		ifStat->condition->visit(this);
		buff << " then\n";

		indent++;
		for (const auto& stat : ifStat->thenbody->as<Parser::AstStatBlock>()->body)
		{
			stat->visit(this);
		}
		indent--;

		if (ifStat->elsebody)
		{
			writeIndent();
			if (auto elseIfStat = ifStat->elsebody->as<Parser::AstStatIf>())
			{
				buff << "elseif ";
				visitIf(elseIfStat);
				return;
			}
			buff << "else\n";
			indent++;
			for (const auto& stat : ifStat->elsebody->as<Parser::AstStatBlock>()->body)
			{
				stat->visit(this);
			}
			indent--;
		}
	}
public:
	CodeVisitor(std::ostream& buff) : buff(buff)
	{
		buff.precision(14);
	}

	bool visit(Parser::AstExpr* expr) override
	{
		buff << "--[[ unknown ]]";
		return false;
	}

	bool visit(Parser::AstStat* expr) override
	{
		writeIndent();
		buff << "-- unknown\n";
		return false;
	}

	bool visit(Parser::AstExprGroup* groupExpr) override
	{
		buff << "(";
		groupExpr->expr->visit(this);
		buff << ")";
		return false;
	}

	bool visit(Parser::AstExprConstantNil*) override
	{
		buff << "nil";
		return false;
	}

	bool visit(Parser::AstExprConstantBool* boolExpr) override
	{
		buff << (boolExpr->value ? "true" : "false");
		return false;
	}

	bool visit(Parser::AstExprConstantNumber* numExpr) override
	{
		buff << numExpr->value;
		return false;
	}

	bool visit(Parser::AstExprConstantString* strExpr) override
	{
		auto str = std::string{ strExpr->value.data, strExpr->value.size };
		switch (getStringQuoteType(str))
		{
		case StringQuoteType::Single:
			buff << '\'' << str << '\'';
			break;
		case StringQuoteType::Double:
			buff << '"' << str << '"';
			break;
		case StringQuoteType::Escape:
		case StringQuoteType::Long:
			buff << "[[" << str << "]]";
			break;
		}

		return false;
	}

	bool visit(Parser::AstExprLocal* localExpr) override
	{
		buff << localExpr->local->name.value;
		return false;
	}

	bool visit(Parser::AstExprGlobal* globalExpr) override
	{
		buff << globalExpr->name.value;
		return false;
	}

	bool visit(Parser::AstExprVarargs*) override
	{
		buff << "...";
		return false;
	}

	bool visit(Parser::AstExprCall* callExpr) override
	{
		if (callExpr->self && callExpr->func->is<Parser::AstExprIndexName>())
		{
			auto indexNameExpr = callExpr->func->as<Parser::AstExprIndexName>();

			indexNameExpr->expr->visit(this);
			buff << ":" << indexNameExpr->index.value;
		}
		else
		{
			bool noParen =
				callExpr->func->is<Parser::AstExprLocal>()
				|| callExpr->func->is<Parser::AstExprGlobal>()
				|| callExpr->func->is<Parser::AstExprGroup>()
				|| callExpr->func->is<Parser::AstExprIndexName>()
				|| callExpr->func->is<Parser::AstExprIndexExpr>();
			if (!noParen)
			{
				buff << "(";
			}
			callExpr->func->visit(this);
			if (!noParen)
			{
				buff << ")";
			}
		}
		
		buff << "(";
		for (size_t i = 0; i < callExpr->args.size; ++i)
		{
			auto expr = callExpr->args.data[i];
			expr->visit(this);

			if (i != callExpr->args.size - 1)
			{
				buff << ", ";
			}
		}
		buff << ")";

		return false;
	}

	bool visit(Parser::AstExprIndexName* indexNameExpr) override
	{
		indexNameExpr->expr->visit(this);
		buff << "." << indexNameExpr->index.value;

		return false;
	}

	bool visit(Parser::AstExprIndexExpr* indexExpr) override
	{
		indexExpr->expr->visit(this);
		if (auto strExpr = indexExpr->index->as<Parser::AstExprConstantString>())
		{
			std::string str{ strExpr->value.data, strExpr->value.size };
			if (isValidName(str))
			{
				buff << "." << str;
				return false;
			}
		}
		buff << "[";
		indexExpr->index->visit(this);
		buff << "]";

		return false;
	}

	bool visit(Parser::AstExprFunction* funcExpr) override
	{
		buff << "function(";
		for (size_t i = 0; i < funcExpr->args.size; ++i)
		{
			auto expr = funcExpr->args.data[i];
			buff << expr->name.value;

			if (i != funcExpr->args.size - 1)
			{
				buff << ", ";
			}
		}

		if (funcExpr->vararg)
		{
			if (funcExpr->args.size > 0)
			{
				buff << ", ";
			}
			buff << "...";
		}
		buff << ")\n";

		indent++;
		for (const auto& stat : funcExpr->body->as<Parser::AstStatBlock>()->body)
		{
			stat->visit(this);
		}
		indent--;

		writeIndent();
		buff << "end";

		return false;
	}

	bool visit(Parser::AstExprTable* tableExpr) override
	{
		buff << "{";

		if (tableExpr->pairs.size > 0)
		{
			indent++;

			for (size_t i = 0; i < tableExpr->pairs.size; i += 2)
			{
				if (i % 30 * 2 == 0)
				{
					buff << "\n";
					writeIndent();
				}

				auto k = tableExpr->pairs.data[i];
				auto v = tableExpr->pairs.data[i + 1];

				if (k) // put on new line?
				{
					if (auto strExpr = k->as<Parser::AstExprConstantString>())
					{
						auto strVal = std::string{ strExpr->value.data, strExpr->value.size };
						if (isValidName(strVal))
						{
							buff << strVal << " = ";
							goto end;
						}
					}
					buff << "[";
					k->visit(this);
					buff << "] = ";
				}
			end:
				v->visit(this);

				if (i != tableExpr->pairs.size - 2)
				{
					buff << ", ";
				}
				else
				{
					buff << "\n";
				}
			}

			indent--;

			writeIndent();
		}

		buff << "}";

		return false;
	}

	bool visit(Parser::AstExprUnary* unaryExpr) override
	{
		switch (unaryExpr->op)
		{
		case Parser::AstExprUnary::Not:
			buff << "not ";
			break;
		case Parser::AstExprUnary::Minus: // TODO: rename to negate?
			buff << "-";
			break;
		case Parser::AstExprUnary::Len:
			buff << "#";
			break;
		default: ;
		}

		unaryExpr->expr->visit(this);

		return false;
	}

	bool visit(Parser::AstExprBinary* binaryExpr) override
	{
		binaryExpr->left->visit(this);

		switch (binaryExpr->op)
		{
		case Parser::AstExprBinary::Add:
			buff << " + ";
			break;
		case Parser::AstExprBinary::Sub:
			buff << " - ";
			break;
		case Parser::AstExprBinary::Mul:
			buff << " * "; 
			break;
		case Parser::AstExprBinary::Div:
			buff << " / ";
			break;
		case Parser::AstExprBinary::Mod:
			buff << " % ";
			break;
		case Parser::AstExprBinary::Pow:
			buff << " ^ ";
			break;
		case Parser::AstExprBinary::Concat:
			buff << " .. ";
			break;
		case Parser::AstExprBinary::CompareNe:
			buff << " ~= ";
			break;
		case Parser::AstExprBinary::CompareEq:
			buff << " == ";
			break;
		case Parser::AstExprBinary::CompareLt:
			buff << " < ";
			break;
		case Parser::AstExprBinary::CompareLe:
			buff << " <= ";
			break;
		case Parser::AstExprBinary::CompareGt:
			buff << " > ";
			break;
		case Parser::AstExprBinary::CompareGe:
			buff << " >= ";
			break;
		case Parser::AstExprBinary::And:
			buff << " and ";
			break;
		case Parser::AstExprBinary::Or:
			buff << " or ";
			break;
		default: ;
		}

		binaryExpr->right->visit(this);

		return false;
	}

	bool visit(Parser::AstStatBlock* blockStat) override
	{
		bool wasMainEncountered = mainEncountered;

		mainEncountered = true;

		if (wasMainEncountered)
		{
			writeIndent();
			buff << "do";
		}

		if (blockStat->body.size)
		{
			if (wasMainEncountered)
			{
				buff << '\n';
				indent++;
			}

			for (const auto& stat : blockStat->body)
			{
				stat->visit(this);
			}

			if (wasMainEncountered)
			{
				indent--;
				writeIndent();
			}
		}
		else
		{
			buff << ' ';
		}


		if (wasMainEncountered)
		{
			buff << "end\n";
		}

		return false;
	}

	bool visit(Parser::AstStatIf* ifStat) override
	{
		writeIndent();
		buff << "if ";

		visitIf(ifStat);

		writeIndent();
		buff << "end\n";

		// TODO: else and elseif

		return false;
	}

	bool visit(Parser::AstStatWhile* whileStat) override
	{
		writeIndent();
		buff << "while ";
		whileStat->condition->visit(this);
		buff << " do\n";

		indent++;
		for (const auto& stat : whileStat->body->as<Parser::AstStatBlock>()->body)
		{
			stat->visit(this);
		}
		indent--;

		writeIndent();
		buff << "end\n";

		return false;
	}

	bool visit(Parser::AstStatRepeat* repeatStat) override
	{
		writeIndent();
		buff << "repeat\n";

		indent++;
		for (const auto& stat : repeatStat->body->as<Parser::AstStatBlock>()->body)
		{
			stat->visit(this);
		}
		indent--;

		writeIndent();
		buff << "until ";
		repeatStat->condition->visit(this);
		buff << "\n";

		return false;
	}

	bool visit(Parser::AstStatBreak* breakStat) override
	{
		writeIndent();
		buff << "break\n";
		return false;
	}

	bool visit(Parser::AstStatReturn* retStat) override
	{
		writeIndent();
		buff << "return ";
		for (size_t i = 0; i < retStat->list.size; ++i)
		{
			auto expr = retStat->list.data[i];
			expr->visit(this);

			if (i != retStat->list.size - 1)
			{
				buff << ", ";
			}
		}
		buff << "\n";

		return false;
	}

	bool visit(Parser::AstStatExpr* exprStat) override
	{
		writeIndent();
		exprStat->expr->visit(this);
		buff << "\n";

		return false;
	}

	bool visit(Parser::AstStatLocalFunction* localFuncStat) override
	{
		writeIndent();
		buff << "local function " << localFuncStat->var->name.value << "(";

		auto funcExpr = localFuncStat->body->as<Parser::AstExprFunction>();
		for (size_t i = 0; i < funcExpr->args.size; ++i)
		{
			auto expr = funcExpr->args.data[i];
			buff << expr->name.value;

			if (i != funcExpr->args.size - 1)
			{
				buff << ", ";
			}
		}
		if (funcExpr->vararg)
		{
			if (funcExpr->args.size > 0)
			{
				buff << ", ";
			}
			buff << "...";
		}

		buff << ")\n";

		indent++;
		for (const auto& stat : funcExpr->body->as<Parser::AstStatBlock>()->body)
		{
			stat->visit(this);
		}
		indent--;

		writeIndent();
		buff << "end\n";

		return false;
	}

	bool visit(Parser::AstStatLocal* localStat) override
	{
		writeIndent();
		buff << "local ";
		for (size_t i = 0; i < localStat->vars.size; ++i)
		{
			auto expr = localStat->vars.data[i];
			buff << expr->name.value;
			localStat->vars.data[i]->utilized = true;

			if (i != localStat->vars.size - 1)
			{
				buff << ", ";
			}
		}

		if (localStat->values.size > 0)
		{
			if (localStat->values.size == 1)
			{
				if (localStat->values.data[0]->is<Parser::AstExprConstantNil>())
				{
					localStat->vars.data[0]->utilized = true;
					buff << "\n";
					return false;
				}
			}
			buff << " = ";

			for (size_t i = 0; i < localStat->values.size; ++i)
			{
				auto expr = localStat->values.data[i];
				expr->visit(this);
				localStat->vars.data[i]->utilized = true;

				if (i != localStat->values.size - 1)
				{
					buff << ", ";
				}
			}
		}

		buff << "\n";

		return false;
	}

	bool visit(Parser::AstStatFor* forStat) override
	{
		writeIndent();
		buff << "for " << forStat->var->name.value << " = ";
		forStat->from->visit(this);
		buff << ", ";
		forStat->to->visit(this);

		if (forStat->step)
		{
			buff << ", ";
			forStat->step->visit(this);
		}

		buff << " do\n";

		indent++;
		for (const auto& stat : forStat->body->as<Parser::AstStatBlock>()->body)
		{
			stat->visit(this);
		}
		indent--;

		writeIndent();
		buff << "end\n";

		return false;
	}

	bool visit(Parser::AstStatForIn* forInStat) override
	{
		writeIndent();
		buff << "for ";
		for (size_t i = 0; i < forInStat->vars.size; ++i)
		{
			auto expr = forInStat->vars.data[i];
			buff << expr->name.value;

			if (i != forInStat->vars.size - 1)
			{
				buff << ", ";
			}
		}

		buff << " in ";

		for (size_t i = 0; i < forInStat->values.size; ++i)
		{
			auto expr = forInStat->values.data[i];
			expr->visit(this);

			if (i != forInStat->values.size - 1)
			{
				buff << ", ";
			}
		}

		buff << " do\n";

		indent++;
		for (const auto& stat : forInStat->body->as<Parser::AstStatBlock>()->body)
		{
			stat->visit(this);
		}
		indent--;

		writeIndent();
		buff << "end\n";

		return false;
	}

	bool visit(Parser::AstStatFunction* funcStat) override
	{
		writeIndent();
		auto funcExpr = funcStat->body->as<Parser::AstExprFunction>();

		buff << "function ";
		if (funcExpr->self && funcStat->expr->is<Parser::AstExprIndexName>())
		{
			auto indexNameExpr = funcStat->expr->as<Parser::AstExprIndexName>();

			indexNameExpr->expr->visit(this);
			buff << ":" << indexNameExpr->index.value;
		}
		else
		{
			funcStat->expr->visit(this);
		}

		buff << "(";


		for (size_t i = 0; i < funcExpr->args.size; ++i)
		{
			auto expr = funcExpr->args.data[i];
			buff << expr->name.value;

			if (i != funcExpr->args.size - 1)
			{
				buff << ", ";
			}
		}
		if (funcExpr->vararg)
		{
			if (funcExpr->args.size > 0)
			{
				buff << ", ";
			}
			buff << "...";
		}

		buff << ")\n";

		indent++;
		for (const auto& stat : funcExpr->body->as<Parser::AstStatBlock>()->body)
		{
			stat->visit(this);
		}
		indent--;

		writeIndent();
		buff << "end\n";

		return false;
	}

	bool visit(Parser::AstStatAssign* assignStat) override
	{
		writeIndent();
		for (size_t i = 0; i < assignStat->vars.size; ++i)
		{
			auto expr = assignStat->vars.data[i];
			expr->visit(this);

			if (i != assignStat->vars.size - 1)
			{
				buff << ", ";
			}
		}

		buff << " = ";

		for (size_t i = 0; i < assignStat->values.size; ++i)
		{
			auto expr = assignStat->values.data[i];
			expr->visit(this);

			if (i != assignStat->values.size - 1)
			{
				buff << ", ";
			}
		}

		buff << "\n";

		return false;
	}
};

void Luau::formatAst(std::ostream& buff, Parser::AstStat* root)
{
	try
	{
		CodeVisitor visitor{ buff };
		root->visit(&visitor);
	}
	catch (...)
	{
		std::rethrow_exception(std::current_exception());
	}
}

void Luau::formatCode(std::ostream& buff, const std::string& source)
{
	try
	{
		Parser::Allocator a;
		Parser::AstNameTable names{ a };
		Parser::AstStat* root =
			Parser::parse(source.data(), source.size(), names, a);

		CodeVisitor visitor{ buff };
		root->visit(&visitor);
	}
	catch (...)
	{
		std::rethrow_exception(std::current_exception());
	}
}
