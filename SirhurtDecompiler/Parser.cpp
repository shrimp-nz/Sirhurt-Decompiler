#include "Parser.h"

#include <cstddef>
#include <algorithm>
#include <cassert>
#include <optional>
#include <map>

namespace Luau::Parser
{
	class Lexer
	{
	public:
		Lexer(const char* buffer, size_t bufferSize, AstNameTable& names, Allocator& allocator)
			: buffer(buffer)
			, bufferSize(bufferSize)
			, offset(0)
			, line(1)
			, lineOffset(0)
			, lexeme(Location(Position(0, 0), 0), Lexeme::Eof)
			, names(names)
			, allocator(allocator)
		{
			static_assert(sizeof(kReserved) / sizeof(kReserved[0]) == Lexeme::Reserved_END - Lexeme::Reserved_BEGIN);

			for (int i = Lexeme::Reserved_BEGIN; i < Lexeme::Reserved_END; ++i)
				names.addStatic(kReserved[i - Lexeme::Reserved_BEGIN], static_cast<Lexeme::Type>(i));

			// read first lexeme
			next();
		}

		const Lexeme& next()
		{
			// consume whitespace or comments before the token
			while (isSpace(peekch()) || (peekch(0) == '-' && peekch(1) == '-'))
			{
				if (peekch(0) == '-')
				{
					consume();
					consume();

					skipCommentBody();
				}
				else
				{
					while (isSpace(peekch()))
						consume();
				}
			}

			lexeme = readNext();

			return lexeme;
		}

		const Lexeme& current() const
		{
			return lexeme;
		}

	private:
		static bool isNewline(char ch)
		{
			return ch == '\n';
		}

		static bool isSpace(char ch)
		{
			return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
		}

		static bool isAlpha(char ch)
		{
			return static_cast<unsigned int>(ch - 'A') < 26 || static_cast<unsigned int>(ch - 'a') < 26;
		}

		static bool isDigit(char ch)
		{
			return static_cast<unsigned int>(ch - '0') < 10;
		}

		static char unescape(char ch)
		{
			switch (ch)
			{
			case 'a': return '\a';
			case 'b': return '\b';
			case 'f': return '\f';
			case 'n': return '\n';
			case 'r': return '\r';
			case 't': return '\t';
			case 'v': return '\v';
			default: return ch;
			}
		}

		char peekch() const
		{
			return (offset < bufferSize) ? buffer[offset] : 0;
		}

		char peekch(unsigned int lookahead) const
		{
			return (offset + lookahead < bufferSize) ? buffer[offset + lookahead] : 0;
		}

		Position position() const
		{
			return Position(line, offset - lineOffset);
		}

		void consume()
		{
			if (isNewline(buffer[offset]))
			{
				line++;
				lineOffset = offset + 1;
			}

			offset++;
		}

		void skipCommentBody()
		{
			if (peekch() == '[')
			{
				int sep = skipLongSeparator();

				if (sep >= 0)
				{
					Position start = position();

					if (!readLongString(scratchData, sep))
						throw ParseError(Location(start, position()), "unfinished long comment near %s", next().toString().c_str());

					return;
				}
			}

			// fall back to single-line comment
			while (peekch() != 0 && !isNewline(peekch()))
				consume();
		}

		// Given a sequence [===[ or ]===], returns:
		// 1. number of equal signs (or 0 if none present) between the brackets
		// 2. -1 if this is not a long comment/string separator
		// 3. -N if this is a malformed separator
		// Does *not* consume the closing brace.
		int skipLongSeparator()
		{
			char start = peekch();

			assert(start == '[' || start == ']');
			consume();

			int count = 0;

			while (peekch() == '=')
			{
				consume();
				count++;
			}

			return (start == peekch()) ? count : (-count) - 1;
		}

		bool readLongString(std::string& data, int sep)
		{
			data.clear();

			// skip (second) [
			assert(peekch() == '[');
			consume();

			// skip first newline
			if (isNewline(peekch()))
				consume();

			unsigned int startOffset = offset;

			while (peekch())
			{
				if (peekch() == ']')
				{
					if (skipLongSeparator() == sep)
					{
						assert(peekch() == ']');
						consume(); // skip (second) ]

						data.assign(buffer + startOffset, buffer + offset - sep - 2);
						return true;
					}
				}
				else
				{
					consume();
				}
			}

			return false;
		}

		char readEscapedChar(const Position& start)
		{
			switch (peekch())
			{
			case '\n':
				consume();
				return '\n';

			case '\r':
				consume();
				if (peekch() == '\n')
					consume();

				return '\n';

			case 0:
				throw ParseError(Location(start, position()), "unfinished string near %s", next().toString().c_str());

			default:
			{
				if (isDigit(peekch()))
				{
					int code = 0;
					int i = 0;

					do
					{
						code = 10 * code + (peekch() - '0');
						consume();
					} while (++i < 3 && isDigit(peekch()));

					if (code > UCHAR_MAX)
						throw ParseError(Location(start, position()), "Escape sequence too large");

					return static_cast<char>(code);
				}
				else
				{
					char result = unescape(peekch());
					consume();

					return result;
				}
			}
			}
		}

		void readString(std::string& data)
		{
			Position start = position();

			char delimiter = peekch();
			assert(delimiter == '\'' || delimiter == '"');
			consume();

			data.clear();

			while (peekch() != delimiter)
			{
				switch (peekch())
				{
				case 0:
				case '\r':
				case '\n':
					throw ParseError(Location(start, position()), "unfinished string near %s", next().toString().c_str());

				case '\\':
					consume();
					data += readEscapedChar(start);
					break;

				default:
					data += peekch();
					consume();
				}
			}

			consume();
		}

		void readNumber(std::string& data, unsigned int startOffset)
		{
			assert(isDigit(peekch()));

			// This function does not do the number parsing - it only skips a number-like pattern.
			// It uses the same logic as Lua stock lexer; the resulting string is later converted
			// to a number with proper verification.
			do
			{
				consume();
			} while (isDigit(peekch()) || peekch() == '.');

			if (peekch() == 'e' || peekch() == 'E')
			{
				consume();

				if (peekch() == '+' || peekch() == '-')
					consume();
			}

			while (isAlpha(peekch()) || isDigit(peekch()) || peekch() == '_')
				consume();

			data.assign(buffer + startOffset, buffer + offset);
		}

		Lexeme readNext()
		{
			Position start = position();

			switch (peekch())
			{
			case 0:
				return Lexeme(Location(start, 0), Lexeme::Eof);

			case '-':
				consume();

				return Lexeme(Location(start, 1), '-');

			case '[':
			{
				int sep = skipLongSeparator();

				if (sep >= 0)
				{
					if (!readLongString(scratchData, sep))
						throw ParseError(Location(start, position()), "unfinished long string near %s", next().toString().c_str());

					return Lexeme(Location(start, position()), Lexeme::String, &scratchData);
				}
				else if (sep == -1)
					return Lexeme(Location(start, 1), '[');
				else
					throw ParseError(Location(start, position()), "Invalid long string delimiter");
			}

			case '=':
			{
				consume();

				if (peekch() == '=')
				{
					consume();
					return Lexeme(Location(start, 2), Lexeme::Equal);
				}
				else
					return Lexeme(Location(start, 1), '=');
			}

			case '<':
			{
				consume();

				if (peekch() == '=')
				{
					consume();
					return Lexeme(Location(start, 2), Lexeme::LessEqual);
				}
				else
					return Lexeme(Location(start, 1), '<');
			}

			case '>':
			{
				consume();

				if (peekch() == '=')
				{
					consume();
					return Lexeme(Location(start, 2), Lexeme::GreaterEqual);
				}
				else
					return Lexeme(Location(start, 1), '>');
			}

			case '~':
			{
				consume();

				if (peekch() == '=')
				{
					consume();
					return Lexeme(Location(start, 2), Lexeme::NotEqual);
				}
				else
					return Lexeme(Location(start, 1), '~');
			}

			case '"':
			case '\'':
				readString(scratchData);

				return Lexeme(Location(start, position()), Lexeme::String, &scratchData);

			case '.':
				consume();

				if (peekch() == '.')
				{
					consume();

					if (peekch() == '.')
					{
						consume();

						return Lexeme(Location(start, 3), Lexeme::Dot3);
					}
					else
						return Lexeme(Location(start, 2), Lexeme::Dot2);
				}
				else
				{
					if (isDigit(peekch()))
					{
						readNumber(scratchData, offset - 1);

						return Lexeme(Location(start, position()), Lexeme::Number, &scratchData);
					}
					else
						return Lexeme(Location(start, 1), '.');
				}

			default:
				if (isDigit(peekch()))
				{
					readNumber(scratchData, offset);

					return Lexeme(Location(start, position()), Lexeme::Number, &scratchData);
				}
				else if (isAlpha(peekch()) || peekch() == '_')
				{
					unsigned int startOffset = offset;

					do consume();
					while (isAlpha(peekch()) || isDigit(peekch()) || peekch() == '_');

					scratchData.assign(buffer + startOffset, buffer + offset);

					std::pair<AstName, Lexeme::Type> name = names.getOrAddWithType(scratchData.c_str());

					if (name.second == Lexeme::Name)
						return Lexeme(Location(start, position()), Lexeme::Name, name.first.value);
					else
						return Lexeme(Location(start, position()), name.second);
				}
				else
				{
					char ch = peekch();
					consume();

					return Lexeme(Location(start, 1), ch);
				}
			}
		}

		const char* buffer;
		size_t bufferSize;

		unsigned int offset;

		unsigned int line;
		unsigned int lineOffset;

		std::string scratchData;

		Lexeme lexeme;

		AstNameTable& names;

		Allocator& allocator;
	};

	template <typename T> class TempVector
	{
	public:
		explicit TempVector(std::vector<T>& storage)
			: storage(storage)
			, offset(storage.size())
		{
		}

		~TempVector()
		{
			assert(storage.size() >= offset);
			storage.erase(storage.begin() + offset, storage.end());
		}

		const T& operator[](size_t index) const
		{
			return storage[offset + index];
		}

		const T& front() const
		{
			return storage[offset];
		}

		const T& back() const
		{
			return storage.back();
		}

		bool empty() const
		{
			return storage.size() == offset;
		}

		size_t size() const
		{
			return storage.size() - offset;
		}

		void push_back(const T& item)
		{
			storage.push_back(item);
		}

	private:
		std::vector<T>& storage;
		unsigned int offset;
	};

	class Parser
	{
	public:
		static AstStat* parse(const char* buffer, size_t bufferSize, AstNameTable& names, Allocator& allocator)
		{
			Parser p(buffer, bufferSize, names, allocator);

			return p.parseChunk();
		}

	private:
		struct Name;

		Parser(const char* buffer, size_t bufferSize, AstNameTable& names, Allocator& allocator)
			: lexer(buffer, bufferSize, names, allocator)
			, allocator(allocator)
		{
			Function top;
			top.vararg = true;

			functionStack.push_back(top);

			nameSelf = names.addStatic("self");
		}

		bool blockFollow(const Lexeme& l)
		{
			return
				l.type == Lexeme::Eof ||
				l.type == Lexeme::ReservedElse ||
				l.type == Lexeme::ReservedElseif ||
				l.type == Lexeme::ReservedEnd ||
				l.type == Lexeme::ReservedUntil;
		}

		AstStat* parseChunk()
		{
			AstStat* result = parseBlock();

			expect(Lexeme::Eof);

			return result;
		}

		// chunk ::= {stat [`;']} [laststat [`;']]
		// block ::= chunk
		AstStat* parseBlock()
		{
			unsigned int localsBegin = saveLocals();

			AstStat* result = parseBlockNoScope();

			restoreLocals(localsBegin);

			return result;
		}

		AstStat* parseBlockNoScope()
		{
			TempVector<AstStat*> body(scratchStat);

			while (!blockFollow(lexer.current()))
			{
				std::pair<AstStat*, bool> stat = parseStat();

				if (lexer.current().type == ';')
					lexer.next();

				body.push_back(stat.first);

				if (stat.second)
					break;
			}

			Location location =
				body.empty()
				? lexer.current().location
				: Location(body.front()->location, body.back()->location);

			return new (allocator) AstStatBlock(location, copy(body));
		}

		// stat ::=
			// varlist `=' explist |
			// functioncall |
			// do block end |
			// while exp do block end |
			// repeat block until exp |
			// if exp then block {elseif exp then block} [else block] end |
			// for Name `=' exp `,' exp [`,' exp] do block end |
			// for namelist in explist do block end |
			// function funcname funcbody |
			// local function Name funcbody |
			// local namelist [`=' explist]
		// laststat ::= return [explist] | break
		std::pair<AstStat*, bool> parseStat()
		{
			switch (lexer.current().type)
			{
			case Lexeme::ReservedIf:
				return std::make_pair(parseIf(), false);
			case Lexeme::ReservedWhile:
				return std::make_pair(parseWhile(), false);
			case Lexeme::ReservedDo:
				return std::make_pair(parseDo(), false);
			case Lexeme::ReservedFor:
				return std::make_pair(parseFor(), false);
			case Lexeme::ReservedRepeat:
				return std::make_pair(parseRepeat(), false);
			case Lexeme::ReservedFunction:
				return std::make_pair(parseFunctionStat(), false);
			case Lexeme::ReservedLocal:
				return std::make_pair(parseLocal(), false);
			case Lexeme::ReservedReturn: // TODO: is this needed?
				return std::make_pair(parseReturn(), true);
			case Lexeme::ReservedBreak:
				return std::make_pair(parseBreak(), true);
			default:
				return std::make_pair(parseAssignmentOrCall(), false);
			}
		}

		// if exp then block {elseif exp then block} [else block] end
		AstStat* parseIf()
		{
			Location start = lexer.current().location;

			lexer.next(); // if / elseif

			AstExpr* cond = parseExpr();

			Lexeme matchThenElse = lexer.current();
			expect(Lexeme::ReservedThen);
			lexer.next();

			AstStat* thenbody = parseBlock();

			AstStat* elsebody = nullptr;
			Location end = start;

			const auto elseType = lexer.current().type;
			if (elseType == Lexeme::ReservedElseif)
			{
				elsebody = parseIf();
				end = elsebody->location;
			}
			else
			{
				if (elseType == Lexeme::ReservedElse)
				{
					matchThenElse = lexer.current();
					lexer.next();

					elsebody = parseBlock();

					// TODO: do we need this?!
					// ReSharper disable once CppIfCanBeReplacedByConstexprIf
					if (true) // (FFlag::StudioVariableIntellesense)
						elsebody->location.begin = matchThenElse.location.end;
				}

				end = lexer.current().location;

				expectMatch(Lexeme::ReservedEnd, matchThenElse);
				lexer.next();
			}

			auto constEvalRes = cond->constEval();
			if (constEvalRes == ConstEvalResult::True)
			{
#ifdef _DEBUG
				// printf("condition constant value evaluated to true\n");
#endif
				return thenbody;
			}
			if (constEvalRes == ConstEvalResult::False)
			{
#ifdef _DEBUG
				// printf("condition constant value evaluated to false\n");
#endif
				if (elseType == Lexeme::ReservedElse
					|| elseType == Lexeme::ReservedElseif)
				{
					return elsebody;
				}
				return new (allocator) AstStatBlock{ Location{start, end}, {} };
			}

#ifdef _DEBUG
			// printf("could not optimize!\n");
#endif

			return new (allocator) AstStatIf(Location(start, end), cond, thenbody, elsebody);
		}

		// while exp do block end
		AstStat* parseWhile()
		{
			Location start = lexer.current().location;

			lexer.next(); // while

			AstExpr* cond = parseExpr();

			Lexeme matchDo = lexer.current();
			expect(Lexeme::ReservedDo);
			lexer.next();

			functionStack.back().loopDepth++;

			AstStat* body = parseBlock();

			functionStack.back().loopDepth--;

			Location end = lexer.current().location;

			expectMatch(Lexeme::ReservedEnd, matchDo);
			lexer.next();

			return new (allocator) AstStatWhile(Location(start, end), cond, body);
		}

		// repeat block until exp
		AstStat* parseRepeat()
		{
			Location start = lexer.current().location;

			Lexeme matchRepeat = lexer.current();
			lexer.next(); // repeat

			unsigned int localsBegin = saveLocals();

			functionStack.back().loopDepth++;

			AstStat* body = parseBlockNoScope();

			functionStack.back().loopDepth--;

			expectMatch(Lexeme::ReservedUntil, matchRepeat);
			lexer.next();

			AstExpr* cond = parseExpr();

			restoreLocals(localsBegin);

			return new (allocator) AstStatRepeat(Location(start, cond->location), cond, body);
		}

		// do block end
		AstStat* parseDo()
		{
			Lexeme matchDo = lexer.current();
			lexer.next(); // do

			AstStat* body = parseBlock();

			expectMatch(Lexeme::ReservedEnd, matchDo);
			lexer.next();

			return body;
		}

		// break
		AstStat* parseBreak()
		{
			if (functionStack.back().loopDepth > 0)
			{
				Location start = lexer.current().location;

				lexer.next(); // break

				return new (allocator) AstStatBreak(start);
			}
			else
				throw ParseError(lexer.current().location, "No loop to break");
		}

		// for Name `=' exp `,' exp [`,' exp] do block end |
		// for namelist in explist do block end |
		AstStat* parseFor()
		{
			Location start = lexer.current().location;

			lexer.next(); // for

			Name varname = parseName();

			if (lexer.current().type == '=')
			{
				lexer.next();

				AstExpr* from = parseExpr();

				expect(',');
				lexer.next();

				AstExpr* to = parseExpr();

				AstExpr* step = NULL;

				if (lexer.current().type == ',')
				{
					lexer.next();

					step = parseExpr();
				}

				Lexeme matchDo = lexer.current();
				expect(Lexeme::ReservedDo);
				lexer.next();

				unsigned int localsBegin = saveLocals();

				AstLocal* var = pushLocal(varname);

				functionStack.back().loopDepth++;

				AstStat* body = parseBlock();

				functionStack.back().loopDepth--;

				restoreLocals(localsBegin);

				Location end = lexer.current().location;

				expectMatch(Lexeme::ReservedEnd, matchDo);
				lexer.next();

				return new (allocator) AstStatFor(Location(start, end), var, from, to, step, body);
			}
			else
			{
				TempVector<Name> names(scratchName);
				names.push_back(varname);

				if (lexer.current().type == ',')
				{
					lexer.next();

					parseNameList(names);
				}

				expect(Lexeme::ReservedIn);
				lexer.next();

				TempVector<AstExpr*> values(scratchExpr);
				parseExprList(values);

				Lexeme matchDo = lexer.current();
				expect(Lexeme::ReservedDo);
				lexer.next();

				unsigned int localsBegin = saveLocals();

				TempVector<AstLocal*> vars(scratchLocal);

				for (size_t i = 0; i < names.size(); ++i)
					vars.push_back(pushLocal(names[i]));

				functionStack.back().loopDepth++;

				AstStat* body = parseBlock();

				functionStack.back().loopDepth--;

				restoreLocals(localsBegin);

				Location end = lexer.current().location;

				expectMatch(Lexeme::ReservedEnd, matchDo);
				lexer.next();

				return new (allocator) AstStatForIn(Location(start, end), copy(vars), copy(values), body);
			}
		}

		// function funcname funcbody |
		// funcname ::= Name {`.' Name} [`:' Name]
		AstStat* parseFunctionStat()
		{
			Location start = lexer.current().location;

			Lexeme matchFunction = lexer.current();
			lexer.next();

			// parse funcname into a chain of indexing operators
			AstExpr* expr = parseNameExpr();

			while (lexer.current().type == '.')
			{
				lexer.next();

				Name name = parseName();

				expr = new (allocator) AstExprIndexName(Location(start, name.location), expr, name.name, name.location);
			}

			// finish with :
			bool hasself = false;

			if (lexer.current().type == ':')
			{
				lexer.next();

				Name name = parseName();

				expr = new (allocator) AstExprIndexName(Location(start, name.location), expr, name.name, name.location);

				hasself = true;
			}

			AstExpr* body = parseFunctionBody(hasself, matchFunction);

			return new (allocator) AstStatFunction(Location(start, body->location), expr, body);
		}

		// local function Name funcbody |
		// local namelist [`=' explist]
		AstStat* parseLocal()
		{
			Location start = lexer.current().location;

			lexer.next(); // local

			if (lexer.current().type == Lexeme::ReservedFunction)
			{
				Lexeme matchFunction = lexer.current();
				lexer.next();

				Name name = parseName();

				AstLocal* var = pushLocal(name);

				AstExpr* body = parseFunctionBody(false, matchFunction);

				return new (allocator) AstStatLocalFunction(Location(start, body->location), var, body);
			}

			TempVector<Name> names(scratchName);
			parseNameList(names);

			TempVector<AstLocal*> vars(scratchLocal);

			TempVector<AstExpr*> values(scratchExpr);

			if (lexer.current().type == '=')
			{
				lexer.next();

				parseExprList(values);
			}

			for (size_t i = 0; i < names.size(); ++i)
				vars.push_back(pushLocal(names[i]));

			Location end = values.empty() ? names.back().location : values.back()->location;

			return new (allocator) AstStatLocal(Location(start, end), copy(vars), copy(values));
		}

		// return [explist]
		AstStat* parseReturn()
		{
			Location start = lexer.current().location;

			lexer.next();

			TempVector<AstExpr*> list(scratchExpr);

			if (!blockFollow(lexer.current()) && lexer.current().type != ';')
				parseExprList(list);

			Location end = list.empty() ? start : list.back()->location;

			return new (allocator) AstStatReturn(Location(start, end), copy(list));
		}

		// varlist `=' explist |
		// functioncall |
		AstStat* parseAssignmentOrCall()
		{
			AstExpr* expr = parsePrimaryExpr();

			if (expr->is<AstExprCall>())
				return new (allocator) AstStatExpr(expr->location, expr);
			else
				return parseAssignment(expr);
		}

		bool isExprVar(AstExpr* expr)
		{
			return
				expr->is<AstExprLocal>() ||
				expr->is<AstExprGlobal>() ||
				expr->is<AstExprIndexExpr>() ||
				expr->is<AstExprIndexName>();
		}

		AstStat* parseAssignment(AstExpr* initial)
		{
			// The initial expr has to be a var
			if (!isExprVar(initial))
				throw ParseError(initial->location, "Syntax error: expression must be a variable or a field");

			TempVector<AstExpr*> vars(scratchExpr);
			vars.push_back(initial);

			while (lexer.current().type == ',')
			{
				lexer.next();

				AstExpr* expr = parsePrimaryExpr();

				if (!isExprVar(expr))
					throw ParseError(expr->location, "Syntax error: expression must be a variable or a field");

				vars.push_back(expr);
			}

			expect('=');
			lexer.next();

			TempVector<AstExpr*> values(scratchExprAux);
			parseExprList(values);

			return new (allocator) AstStatAssign(Location(initial->location, values.back()->location), copy(vars), copy(values));
		}

		AstArray<AstName*> parseAttributeList()
		{
			std::vector<AstName*> attributes{};

			while (true)
			{
				if (lexer.current().type == '[')
				{
					Lexeme matchOpen = lexer.current();
					lexer.next();

					attributes.push_back(new (allocator) AstName(parseName().name));

					expectMatch(']', matchOpen);
					lexer.next();
					continue;
				}
				break;
			}

			return copy(attributes.data(), attributes.size());
		}

		// funcbody ::= `(' [parlist] `)' block end
		// parlist ::= namelist [`,' `...'] | `...'
		AstExprFunction* parseFunctionBody(bool hasself, const Lexeme& matchFunction)
		{
			Location start = lexer.current().location;

			Lexeme matchParen = lexer.current();
			expect('(');
			lexer.next();

			TempVector<Name> args(scratchName);
			bool vararg = false;

			if (lexer.current().type != ')')
				vararg = parseNameList(args, /* allowDot3= */ true);

			expectMatch(')', matchParen);
			lexer.next();

			auto attributes = parseAttributeList();

			unsigned int localsBegin = saveLocals();

			AstLocal* self = NULL;

			if (hasself)
			{
				self = pushLocal(Name(nameSelf, start));
			}

			TempVector<AstLocal*> vars(scratchLocal);

			for (size_t i = 0; i < args.size(); ++i)
				vars.push_back(pushLocal(args[i]));

			Function fun;
			fun.vararg = vararg;

			functionStack.push_back(fun);

			AstStat* body = parseBlock();

			functionStack.pop_back();

			restoreLocals(localsBegin);

			// Location end = lexer.current().location;

			expectMatch(Lexeme::ReservedEnd, matchFunction);
			lexer.next();
			Location end = lexer.current().location;

			return new (allocator) AstExprFunction(Location(start, end), self, copy(vars), vararg, attributes, body);
		}

		// explist ::= {exp `,'} exp
		void parseExprList(TempVector<AstExpr*>& result)
		{
			result.push_back(parseExpr());

			while (lexer.current().type == ',')
			{
				lexer.next();

				result.push_back(parseExpr());
			}
		}

		// namelist ::= Name {`,' Name}
		bool parseNameList(TempVector<Name>& result, bool allowDot3 = false)
		{
			if (lexer.current().type == Lexeme::Dot3 && allowDot3)
			{
				lexer.next();

				return true;
			}

			result.push_back(parseName());

			while (lexer.current().type == ',')
			{
				lexer.next();

				if (lexer.current().type == Lexeme::Dot3 && allowDot3)
				{
					lexer.next();

					return true;
				}

				result.push_back(parseName());
			}

			return false;
		}

		static std::optional<AstExprUnary::Op> parseUnaryOp(const Lexeme& l)
		{
			switch (l.type)
			{
			case Lexeme::ReservedNot:
				return AstExprUnary::Not;
			case '-':
				return AstExprUnary::Minus;
			case '#':
				return AstExprUnary::Len;
			default:
				return {};
			}
		}

		static std::optional<AstExprBinary::Op> parseBinaryOp(const Lexeme& l)
		{
			// TODO: switch statement (yay jumptables)
			if (l.type == '+')
				return AstExprBinary::Add;
			if (l.type == '-')
				return AstExprBinary::Sub;
			if (l.type == '*')
				return AstExprBinary::Mul;
			if (l.type == '/')
				return AstExprBinary::Div;
			if (l.type == '%')
				return AstExprBinary::Mod;
			if (l.type == '^')
				return AstExprBinary::Pow;
			if (l.type == Lexeme::Dot2)
				return AstExprBinary::Concat;
			if (l.type == Lexeme::NotEqual)
				return AstExprBinary::CompareNe;
			if (l.type == Lexeme::Equal)
				return AstExprBinary::CompareEq;
			if (l.type == '<')
				return AstExprBinary::CompareLt;
			if (l.type == Lexeme::LessEqual)
				return AstExprBinary::CompareLe;
			if (l.type == '>')
				return AstExprBinary::CompareGt;
			if (l.type == Lexeme::GreaterEqual)
				return AstExprBinary::CompareGe;
			if (l.type == Lexeme::ReservedAnd)
				return AstExprBinary::And;
			if (l.type == Lexeme::ReservedOr)
				return AstExprBinary::Or;

			return {};
		}

		struct BinaryOpPriority
		{
			unsigned char left, right;
		};

		// subexpr -> (simpleexp | unop subexpr) { binop subexpr }
		// where `binop' is any binary operator with a priority higher than `limit'
		std::pair<AstExpr*, std::optional<AstExprBinary::Op> > parseSubExpr(unsigned int limit)
		{
			static const BinaryOpPriority binaryPriority[] =
			{
				{6, 6}, {6, 6}, {7, 7}, {7, 7}, {7, 7},  // `+' `-' `/' `%'
				{10, 9}, {5, 4},                 // power and concat (right associative)
				{3, 3}, {3, 3},                  // equality and inequality
				{3, 3}, {3, 3}, {3, 3}, {3, 3},  // order
				{2, 2}, {1, 1}                   // logical (and/or)
			};

			const unsigned int unaryPriority = 8;

			Location start = lexer.current().location;

			AstExpr* expr;

			if (std::optional<AstExprUnary::Op> uop = parseUnaryOp(lexer.current()))
			{
				lexer.next();

				AstExpr* subexpr = parseSubExpr(unaryPriority).first;
				AstExprConstantNumber* numExpr = subexpr->as<AstExprConstantNumber>();
				if (uop == AstExprUnary::Op::Minus && numExpr)
				{
					numExpr->value = -numExpr->value;
					expr = numExpr;
				}
				else
				{
					expr = new (allocator) AstExprUnary(Location(start, subexpr->location), uop.value(), subexpr);
				}
			}
			else
			{
				expr = parseSimpleExpr();
			}

			// expand while operators have priorities higher than `limit'
			std::optional<AstExprBinary::Op> op = parseBinaryOp(lexer.current());

			while (op && binaryPriority[op.value()].left > limit)
			{
				lexer.next();

				// read sub-expression with higher priority
				std::pair<AstExpr*, std::optional<AstExprBinary::Op> > next = parseSubExpr(binaryPriority[op.value()].right);

				expr = new (allocator) AstExprBinary(Location(start, next.first->location), op.value(), expr, next.first);
				op = next.second;
			}

			return std::make_pair(expr, op);  // return first untreated operator
		}

		AstExpr* parseExpr()
		{
			return parseSubExpr(0).first;
		}

		// NAME
		AstExpr* parseNameExpr()
		{
			Name name = parseName();

			auto it = localMap.find(name.name.value);
			if (it != localMap.end())
			{
				AstLocal* local = localMap.find(name.name.value)->second;

				if (local)
				{
					return new (allocator) AstExprLocal(name.location, local, local->functionDepth != functionStack.size());
				}
			}
			return new (allocator) AstExprGlobal(name.location, name.name);
		}

		// prefixexp -> NAME | '(' expr ')'
		AstExpr* parsePrefixExpr()
		{
			if (lexer.current().type == '(')
			{
				Location start = lexer.current().location;

				Lexeme matchParen = lexer.current();
				lexer.next();

				AstExpr* expr = parseExpr();

				Location end = lexer.current().location;

				expectMatch(')', matchParen);
				lexer.next();

				return new (allocator) AstExprGroup(Location(start, end), expr);
			}
			else
			{
				return parseNameExpr();
			}
		}

		// primaryexp -> prefixexp { `.' NAME | `[' exp `]' | `:' NAME funcargs | funcargs }
		AstExpr* parsePrimaryExpr()
		{
			Location start = lexer.current().location;

			AstExpr* expr = parsePrefixExpr();

			while (true)
			{
				if (lexer.current().type == '.')
				{
					lexer.next();

					Name index = parseName();

					expr = new (allocator) AstExprIndexName(Location(start, index.location), expr, index.name, index.location);
				}
				else if (lexer.current().type == '[')
				{
					Lexeme matchBracket = lexer.current();
					lexer.next();

					AstExpr* index = parseExpr();

					Location end = lexer.current().location;

					expectMatch(']', matchBracket);
					lexer.next();

					expr = new (allocator) AstExprIndexExpr(Location(start, end), expr, index);
				}
				else if (lexer.current().type == ':')
				{
					lexer.next();

					Name index = parseName();
					AstExpr* func = new (allocator) AstExprIndexName(Location(start, index.location), expr, index.name, index.location);

					expr = parseFunctionArgs(func, true);
				}
				else if (lexer.current().type == '{' || lexer.current().type == '(' || lexer.current().type == Lexeme::String)
				{
					expr = parseFunctionArgs(expr, false);
				}
				else
				{
					break;
				}
			}

			return expr;
		}


		// simpleexp -> NUMBER | STRING | NIL | true | false | ... | constructor | FUNCTION body | primaryexp
		AstExpr* parseSimpleExpr()
		{
			Location start = lexer.current().location;

			if (lexer.current().type == Lexeme::ReservedNil)
			{
				lexer.next();

				return new (allocator) AstExprConstantNil(start);
			}
			else if (lexer.current().type == Lexeme::ReservedTrue)
			{
				lexer.next();

				return new (allocator) AstExprConstantBool(start, true);
			}
			else if (lexer.current().type == Lexeme::ReservedFalse)
			{
				lexer.next();

				return new (allocator) AstExprConstantBool(start, false);
			}
			else if (lexer.current().type == Lexeme::ReservedFunction)
			{
				Lexeme matchFunction = lexer.current();
				lexer.next();

				return parseFunctionBody(false, matchFunction);
			}
			else if (lexer.current().type == Lexeme::Number)
			{
				const char* datap = lexer.current().data->c_str();
				char* dataend = NULL;

				double value = strtod(datap, &dataend);

				// maybe a hexadecimal constant?
				if (*dataend == 'x' || *dataend == 'X')
					value = strtoul(datap, &dataend, 16);

				if (*dataend == 0)
				{
					lexer.next();

					return new (allocator) AstExprConstantNumber(start, value);
				}
				else
					throw ParseError(lexer.current().location, "Malformed number");
			}
			else if (lexer.current().type == Lexeme::String)
			{
				AstArray<char> value = copy(*lexer.current().data);

				lexer.next();

				return new (allocator) AstExprConstantString(start, value);
			}
			else if (lexer.current().type == Lexeme::Dot3)
			{
				if (functionStack.back().vararg)
				{
					lexer.next();

					return new (allocator) AstExprVarargs(start);
				}
				else
					throw ParseError(lexer.current().location, "cannot use '...' outside a vararg function near '...'");
			}
			else if (lexer.current().type == '{')
			{
				return parseTableConstructor();
			}
			else
			{
				return parsePrimaryExpr();
			}
		}

		// args ::=  `(' [explist] `)' | tableconstructor | String
		AstExprCall* parseFunctionArgs(AstExpr* func, bool self)
		{
			if (lexer.current().type == '(')
			{
				if (func->location.end.line != lexer.current().location.begin.line)
					throw ParseError(lexer.current().location, "Ambiguous syntax: this looks like an argument list for a function call, but could also be a start of new statement");

				Lexeme matchParen = lexer.current();
				lexer.next();

				TempVector<AstExpr*> args(scratchExpr);

				if (lexer.current().type != ')')
					parseExprList(args);

				Location end = lexer.current().location;

				expectMatch(')', matchParen);
				lexer.next();

				return new (allocator) AstExprCall(Location(func->location, end), func, copy(args), self);
			}
			else if (lexer.current().type == '{')
			{
				AstExpr* expr = parseTableConstructor();

				return new (allocator) AstExprCall(Location(func->location, expr->location), func, copy(&expr, 1), self);
			}
			else if (lexer.current().type == Lexeme::String)
			{
				AstExpr* expr = new (allocator) AstExprConstantString(lexer.current().location, copy(*lexer.current().data));

				lexer.next();

				return new (allocator) AstExprCall(Location(func->location, expr->location), func, copy(&expr, 1), self);
			}
			else
			{
				throw ParseError(lexer.current().location, "'(', '{' or <string> expected near %s", lexer.current().toString().c_str());
			}
		}

		// tableconstructor ::= `{' [fieldlist] `}'
		// fieldlist ::= field {fieldsep field} [fieldsep]
		// field ::= `[' exp `]' `=' exp | Name `=' exp | exp
		// fieldsep ::= `,' | `;'
		AstExpr* parseTableConstructor()
		{
			TempVector<AstExpr*> pairs(scratchExpr);

			Location start = lexer.current().location;

			Lexeme matchBrace = lexer.current();
			expect('{');
			lexer.next();

			while (lexer.current().type != '}')
			{
				if (lexer.current().type == '[')
				{
					Lexeme matchLocationBracket = lexer.current();
					lexer.next();

					AstExpr* key = parseExpr();

					expectMatch(']', matchLocationBracket);
					lexer.next();

					expect('=');
					lexer.next();

					AstExpr* value = parseExpr();

					pairs.push_back(key);
					pairs.push_back(value);
				}
				else
				{
					AstExpr* expr = parseExpr();

					if (lexer.current().type == '=')
					{
						lexer.next();

						AstName name;

						if (AstExprLocal* e = expr->as<AstExprLocal>())
							name = e->local->name;
						else if (AstExprGlobal* e = expr->as<AstExprGlobal>())
							name = e->name;
						else
							throw ParseError(expr->location, "expected a name, got a complex expression");

						AstArray<char> nameString;
						nameString.data = const_cast<char*>(name.value);
						nameString.size = strlen(name.value);

						AstExpr* key = new (allocator) AstExprConstantString(expr->location, nameString);
						AstExpr* value = parseExpr();

						pairs.push_back(key);
						pairs.push_back(value);
					}
					else
					{
						pairs.push_back(NULL);
						pairs.push_back(expr);
					}
				}

				if (lexer.current().type == ',' || lexer.current().type == ';')
					lexer.next();
				else
					expectMatch('}', matchBrace);
			}

			Location end = lexer.current().location;

			lexer.next();

			return new (allocator) AstExprTable(Location(start, end), copy(pairs));
		}

		// Name
		Name parseName()
		{
			if (lexer.current().type != Lexeme::Name)
				throw ParseError(lexer.current().location, "unexpected symbol near %s", lexer.current().toString().c_str());

			Name result(AstName(lexer.current().name), lexer.current().location);

			lexer.next();

			return result;
		}

		AstLocal* pushLocal(const Name& name)
		{
			AstLocal*& local = localMap[name.name.value];

			local = new (allocator) AstLocal(name.name, name.location, local, functionStack.size());

			localStack.push_back(local);

			return local;
		}

		unsigned int saveLocals()
		{
			return localStack.size();
		}

		void restoreLocals(unsigned int offset)
		{
			for (size_t i = localStack.size(); i > offset; --i)
			{
				AstLocal* l = localStack[i - 1];

				localMap[l->name.value] = l->shadow;
			}

			localStack.resize(offset);
		}

		void expect(char value)
		{
			expect(static_cast<Lexeme::Type>(static_cast<unsigned char>(value)));
		}

		void expect(Lexeme::Type type)
		{
			if (lexer.current().type != type)
				throw ParseError(lexer.current().location, "%s expected near %s", Lexeme(Location(Position(0, 0), 0), type).toString().c_str(), lexer.current().toString().c_str());
		}

		void expectMatch(char value, const Lexeme& begin)
		{
			expectMatch(static_cast<Lexeme::Type>(static_cast<unsigned char>(value)), begin);
		}

		void expectMatch(Lexeme::Type type, const Lexeme& begin)
		{
			if (lexer.current().type != type)
			{
				std::string typeString = Lexeme(Location(Position(0, 0), 0), type).toString();

				if (lexer.current().location.begin.line == begin.location.begin.line)
					throw ParseError(lexer.current().location, "%s expected (to close %s at column %d) near %s", typeString.c_str(), begin.toString().c_str(), begin.location.begin.column + 1, lexer.current().toString().c_str());
				else
					throw ParseError(lexer.current().location, "%s expected (to close %s at line %d) near %s", typeString.c_str(), begin.toString().c_str(), begin.location.begin.line + 1, lexer.current().toString().c_str());
			}
		}

		template <typename T> AstArray<T> copy(const T* data, size_t size)
		{
			AstArray<T> result;

			result.data = size ? new (allocator) T[size] : NULL;
			result.size = size;

			std::copy(data, data + size, result.data);

			return result;
		}

		template <typename T> AstArray<T> copy(const TempVector<T>& data)
		{
			return copy(data.empty() ? NULL : &data[0], data.size());
		}

		AstArray<char> copy(const std::string& data)
		{
			AstArray<char> result = copy(data.c_str(), data.size() + 1);

			result.size = data.size();

			return result;
		}

		struct Function
		{
			bool vararg;
			unsigned int loopDepth;

			Function()
				: vararg(false)
				, loopDepth(0)
			{
			}
		};

		struct Local
		{
			AstLocal* local;
			unsigned int offset;

			Local()
				: local(NULL)
				, offset(0)
			{
			}
		};

		struct Name
		{
			AstName name;
			Location location;

			Name(const AstName& name, const Location& location)
				: name(name)
				, location(location)
			{
			}
		};

		Lexer lexer;
		Allocator& allocator;

		AstName nameSelf;

		std::vector<Function> functionStack;

		std::unordered_map<const char*, AstLocal*> localMap;
		std::vector<AstLocal*> localStack;

		std::vector<AstStat*> scratchStat;
		std::vector<AstExpr*> scratchExpr;
		std::vector<AstExpr*> scratchExprAux;
		std::vector<Name> scratchName;
		std::vector<AstLocal*> scratchLocal;
	};

	AstStat* parse(const char* buffer, size_t bufferSize, AstNameTable& names, Allocator& allocator)
	{
		return Parser::parse(buffer, bufferSize, names, allocator);
	}
}
