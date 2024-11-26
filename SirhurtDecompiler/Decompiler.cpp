#include "Decompiler.h"
#include "Parser.h"

#include <sstream>
#include <string_view>
#include <vector>
#include <iostream>
#include <iomanip>
#include <stack>
#include "CodeFormat.h"

template <typename T>
class TempVector
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

class BytecodeReader
{
	const std::vector<byte>& bytecode;
	size_t pointer = 0;
public:
	BytecodeReader(const std::vector<byte>& bytecode)
		: bytecode(bytecode) {}

	int readInt()
	{
		int res = 0;
		size_t i = 0;
		byte readByte;
		do
		{
			readByte = *(bytecode.data() + pointer++);
			res |= (readByte & 0x7F) << i;
			i += 7;
		} while ((readByte & 0x80u) != 0);

		return res;
	}

	template<typename T>
	T read()
	{
		auto res = *(T*)(bytecode.data() + pointer);
		pointer += sizeof(T);
		return res;
	}

	template<typename T>
	const T* read(size_t c)
	{
		auto res = (T*)(bytecode.data() + pointer);
		pointer += sizeof(T) * c;
		return res;
	}
};

enum class OpCode : byte
{
	Nop,
	SaveCode,
	LoadNil,
	LoadBool, // A B C	R(A) := (Bool)B; pc += C
	LoadShort,
	LoadConst,
	Move,
	GetGlobal,
	SetGlobal,
	GetUpvalue,
	SetUpvalue,
	SaveRegisters,
	GetGlobalConst,
	GetTableIndex,
	SetTableIndex,
	GetTableIndexConstant,
	SetTableIndexConstant,
	GetTableIndexByte,
	SetTableIndexByte,
	Closure,
	Self,
	Call,
	Return,
	Jump,
	LoopJump,
	Test,
	NotTest,
	Equal,
	LesserOrEqual,
	LesserThan,
	NotEqual,
	GreaterThan,
	GreaterOrEqual,
	Add,
	Sub,
	Mul,
	Div,
	Mod,
	Pow,
	AddByte,
	SubByte,
	MulByte,
	DivByte,
	ModByte,
	PowByte,
	Or,
	And,
	OrByte,
	AndByte,
	Concat,
	Not,
	UnaryMinus,
	Len,
	NewTable,
	NewTableConst,
	SetList,
	ForPrep,
	ForLoop,
	TForLoop,
	LoopJumpIPairs,
	TForLoopIPairs,
	LoopJumpNext,
	TForLoopNext,
	LoadVarargs,
	ClearStack,
	ClearStackFull,
	LoadConstLarge,
	FarJump,
	BuiltinCall,
	OPCODE_END
};

struct Instruction
{
	union
	{
		uint32_t encoded = 0;
		struct
		{
			OpCode op;
			byte a;
			union
			{
				struct
				{
					byte b;
					byte c;
				};
				uint16_t b_x;
				int16_t s_b_x;
			};
		};
	};
};

enum class ConstantType : byte
{
	ConstantNil,
	ConstantBoolean,
	ConstantNumber,
	ConstantString,
	ConstantGlobal,
	ConstantHashTable
};

struct Proto
{
	// in order of serialization
	byte maxRegCount;
	byte argCount;
	byte upvalCount;
	byte isVarArg;
	std::vector<Instruction> code;
	std::vector<Luau::Parser::AstExpr*> constants;
	std::vector<Proto*> children;
	std::string_view name;
	std::vector<size_t> lineInfo;
	std::vector<Luau::Parser::AstLocal*> args;
	std::vector<Luau::Parser::AstLocal*> upvalues;
	bool isMain = false;
};

struct LocalData
{
	// statement that defines the local - currently unneeded
	// Luau::Parser::AstStat* definition;
	// statements that reference the local
	std::vector<Luau::Parser::AstStat*> references;
};

class LocalCollector : public Luau::Parser::AstVisitor
{
	Luau::Parser::AstStat* context = nullptr;
public:
	std::unordered_map<Luau::Parser::AstLocal*, LocalData> localInfo;

	bool visit(Luau::Parser::AstExprLocal* node) override
	{
		//if (!node->upvalue)
		{
			auto& data = localInfo[node->local];
			data.references.push_back(context);
		}
		return false;
	}

	bool visit(Luau::Parser::AstStat* node) override
	{
		context = node;
		return true;
	}
};

#define VISIT_CHILD(memRef) { nodeRef = &memRef; memRef->visit(this); }

class LocalInliner : public Luau::Parser::AstVisitor
{
	Luau::Parser::AstLocal* find;
	Luau::Parser::AstExprLocal* found;
	
	Luau::Parser::AstExpr* replace;
public:
	LocalInliner(Luau::Parser::AstLocal* find, Luau::Parser::AstExpr* replace)
		: find(find), replace(replace) {};

	Luau::Parser::AstExpr** nodeRef = nullptr;
	size_t inlineCount = 0;

	virtual bool visit(Luau::Parser::AstExprGroup* node)
	{
		VISIT_CHILD(node->expr);
		return false;
	}

	virtual bool visit(Luau::Parser::AstExprCall* node)
	{
		VISIT_CHILD(node->func);

		for (size_t i = 0; i < node->args.size; ++i)
		{
			VISIT_CHILD(node->args.data[i]);
		}

		return false;
	}

	virtual bool visit(Luau::Parser::AstExprIndexName* node)
	{
		VISIT_CHILD(node->expr);
		return false;
	}

	virtual bool visit(Luau::Parser::AstExprLocal* node)
	{
		if (node->local == find)
		{
			*nodeRef = replace;
			inlineCount++;
		}
		return false;
	}

	virtual bool visit(Luau::Parser::AstExprIndexExpr* node)
	{
		VISIT_CHILD(node->expr);
		VISIT_CHILD(node->index);
		return false;
	}

	virtual bool visit(Luau::Parser::AstExprTable* node)
	{
		return false;
	}

	virtual bool visit(Luau::Parser::AstExprUnary* node)
	{
		VISIT_CHILD(node->expr);
		return false;
	}
	virtual bool visit(Luau::Parser::AstExprBinary* node)
	{
		VISIT_CHILD(node->left);
		VISIT_CHILD(node->right);
		return false;
	}

	virtual bool visit(Luau::Parser::AstStatBlock* node)
	{
		return false;
	}

	virtual bool visit(Luau::Parser::AstStatIf* node)
	{
		VISIT_CHILD(node->condition);
		for (auto stat : node->thenbody->as<Luau::Parser::AstStatBlock>()->body)
		{
			stat->visit(this);
		}

		if (node->elsebody)
		{
			for (auto stat : node->elsebody->as<Luau::Parser::AstStatBlock>()->body)
			{
				stat->visit(this);
			}
		}

		return false;
	}

	virtual bool visit(Luau::Parser::AstStatWhile* node)
	{
		VISIT_CHILD(node->condition);
		for (auto stat : node->body->as<Luau::Parser::AstStatBlock>()->body)
		{
			stat->visit(this);
		}
		return false;
	}

	virtual bool visit(Luau::Parser::AstStatRepeat* node)
	{
		return false;
	}

	virtual bool visit(Luau::Parser::AstStatBreak* node)
	{
		return false;
	}

	virtual bool visit(Luau::Parser::AstStatReturn* node)
	{
		for (size_t i = 0; i < node->list.size; ++i)
		{
			VISIT_CHILD(node->list.data[i]);
		}
		return false;
	}

	virtual bool visit(Luau::Parser::AstStatExpr* node)
	{
		VISIT_CHILD(node->expr);
		return false;
	}

	virtual bool visit(Luau::Parser::AstStatLocal* localStat)
	{
		for (size_t i = 0; i < localStat->values.size; ++i)
		{
			VISIT_CHILD(localStat->values.data[i]);
		}
		return false;
	}

	virtual bool visit(Luau::Parser::AstStatFor* node)
	{
		return false;
	}

	virtual bool visit(Luau::Parser::AstStatForIn* node)
	{
		return false;
	}

	virtual bool visit(Luau::Parser::AstStatAssign* assignStat)
	{
		for (size_t i = 0; i < assignStat->values.size; ++i)
		{
			VISIT_CHILD(assignStat->values.data[i])
		}

		for (size_t i = 0; i < assignStat->vars.size; ++i)
		{
			VISIT_CHILD(assignStat->vars.data[i])
		}
		return false;
	}
};

class Decompiler
{
	Luau::Parser::Allocator& a;
	// Luau::Parser::AstNameTable& names;

	phmap::flat_hash_map<byte, OpCode> opConversionTable;

	bool flagged = false;

	std::vector<std::string_view> stringTable;
	std::vector<Proto*> protos;
	Proto* mainProto = nullptr;

	void setFlagged()
	{
		flagged = true;
	}

	static constexpr byte getClientOp(byte op)
	{
		return byte(227 * op);
	}

	void generateOpConvTable()
	{
		for (byte i = 0; i < byte(OpCode::OPCODE_END); ++i)
		{
			opConversionTable[getClientOp(i)] = OpCode(i);
		}
	}

	template <typename T>
	Luau::Parser::AstArray<T> copy(const T* data, size_t size)
	{
		Luau::Parser::AstArray<T> result{};

		result.data = size ? new (a) T[size] : nullptr;
		result.size = size;

		std::copy(data, data + size, result.data);

		return result;
	}

	template <typename T>
	Luau::Parser::AstArray<T> copy(const TempVector<T>& data)
	{
		return copy(data.empty() ? nullptr : &data[0], data.size());
	}

	template <typename T>
	Luau::Parser::AstArray<T> copy(const std::vector<T>& data)
	{
		return copy(data.empty() ? nullptr : &data[0], data.size());
	}

	std::vector<Luau::Parser::AstStat*> scratchStat;
	std::vector<Luau::Parser::AstExpr*> scratchExpr;
	std::vector<Luau::Parser::AstLocal*> scratchLocal;

	std::vector<Proto*> functionStack;

	using LocalStack = std::unordered_map<uint_fast16_t, Luau::Parser::AstLocal*>;

	uint32_t c = 0;

	Luau::Parser::AstLocal* createLocal(const Luau::Parser::Location& location)
	{
		std::string nameString = "var";
		nameString += std::to_string(c++);

		char* nameData = new (a) char[nameString.length() + 1];
		memcpy(nameData, nameString.c_str(), nameString.length() + 1);

		auto name = Luau::Parser::AstName{ nameData };
		return new (a) Luau::Parser::AstLocal{ name, location,
			nullptr, functionStack.size() };
	}

	std::pair<Luau::Parser::AstLocal*, bool> findOrCreateLocal(LocalStack& localStack,
		const Luau::Parser::Location& location, uint_fast16_t i)
	{
		Luau::Parser::AstLocal* local;
		auto it = localStack.find(i);
		if (it == localStack.cend())
		{
			return { localStack[i] = createLocal(location), true };
		}

		return { it->second, false };
	}

	Luau::Parser::AstStat* generateLocalAssign(
		const Luau::Parser::Location& location, Luau::Parser::AstLocal* local,
		bool created, const Luau::Parser::AstArray<Luau::Parser::AstExpr*>& values)
	{
		if (created)
		{
			return new (a) Luau::Parser::AstStatLocal{ location,
				copy(&local, 1), values };
		}

		Luau::Parser::AstExpr* localExpr =
			new (a) Luau::Parser::AstExprLocal{ location, local,
				false };
		return new (a) Luau::Parser::AstStatAssign{ location,
			copy(&localExpr, 1), values };
	}

	struct ControlFlowInfo
	{
		size_t codeStartIndex;
		size_t bodyStartIndex;
		size_t codeEndIndex;

		Luau::Parser::AstLocal* local;

		enum class Type
		{
			Test,
			NotTest
		} type;

		Luau::Parser::Location location;
	};

	Luau::Parser::AstStatBlock* decompile(Proto* p)
	{
		// TempVector<Luau::Parser::AstStat*> body{ scratchStat };
		std::vector<Luau::Parser::AstStat*> body{};
		LocalStack localStack{};

		for (byte i = 0; i < p->argCount; ++i)
		{
			std::string nameString = "a";
			nameString += std::to_string(i);

			char* nameData = new (a) char[nameString.length() + 1];
			memcpy(nameData, nameString.c_str(), nameString.length() + 1);

			auto name = Luau::Parser::AstName{ nameData };
			auto local = new (a) Luau::Parser::AstLocal{ name, 
				{{0, 0}, {0, 0}}, nullptr,
				functionStack.size() };

			localStack[i] = local;
			p->args.push_back(local);
		}

		bool isTail = false;
		byte tailBase = 0;
		Luau::Parser::AstExpr* tailExpr = nullptr;

		bool callRes = false;
		byte callResBase = 0;
		byte callResCount = 0;

		bool self = false;
		Luau::Parser::AstExpr* selfExpr = nullptr;

		std::deque<ControlFlowInfo> f;

		std::vector<size_t> instrBodyMap{};
		instrBodyMap.reserve(p->code.size());

		for (size_t i = 0; i < p->code.size(); ++i)
		{
			auto instr = p->code[i];

			auto line = p->lineInfo[i];
			Luau::Parser::Position position{ line, 0 };

			instrBodyMap.push_back(body.size());

			switch (instr.op)
			{
			case OpCode::Nop:
			{
				setFlagged();
				break;
			}
			case OpCode::SaveCode: // idk what this even does
			{
				std::cout << "save code?!\n";
				break;
			}
			case OpCode::LoadNil:
			{
				Luau::Parser::Location location = { position, position };
				auto[local, created] =
					findOrCreateLocal(localStack, location, instr.a);
				Luau::Parser::AstExpr* nilExpr =
					new (a) Luau::Parser::AstExprConstantNil{ location };

				auto stat = generateLocalAssign(location, local, created,
					copy(&nilExpr, 1));
				body.push_back(stat);
				break;
			}
			case OpCode::LoadBool:
			{
				Luau::Parser::Location location = { position, position };
				auto[local, created] =
					findOrCreateLocal(localStack, location, instr.a);
				Luau::Parser::AstExpr* boolExpr =
					new (a) Luau::Parser::AstExprConstantBool{ location,
						bool(instr.b) };

				auto stat = generateLocalAssign(location, local, created,
					copy(&boolExpr, 1));
				body.push_back(stat);
				break;
			}
			case OpCode::LoadShort:
			{
				Luau::Parser::Location location = { position, position };
				auto[local, created] =
					findOrCreateLocal(localStack, location, instr.a);
				Luau::Parser::AstExpr* numExpr =
					new (a) Luau::Parser::AstExprConstantNumber{ location,
						double(instr.s_b_x) };

				auto stat = generateLocalAssign(location, local, created,
					copy(&numExpr, 1));
				body.push_back(stat);
				break;
			}
			case OpCode::LoadConst:
			{
				Luau::Parser::Location location = { position, position };
				auto[local, created] =
					findOrCreateLocal(localStack, location, instr.a);
				Luau::Parser::AstExpr* expr =
					p->constants[instr.b_x]; // TODO: copy and set location

				auto stat = generateLocalAssign(location, local, created,
					copy(&expr, 1));
				body.push_back(stat);
				break;
			}
			case OpCode::Move:
			{
				Luau::Parser::Location location = { position, position };
				auto[toLocal, toCreated] =
					findOrCreateLocal(localStack, location, instr.a);

				Luau::Parser::AstExpr* expr;
				if (isTail && instr.b >= tailBase)
				{
					if (instr.b == tailBase)
					{
						expr = tailExpr;
					}
					else
					{
						expr = new (a) Luau::Parser::AstExprConstantNil{ location };
					}
				}
				else
				{
					auto[fromLocal, fromCreated] =
						findOrCreateLocal(localStack, location, instr.b);

					expr = new (a) Luau::Parser::AstExprLocal{ location, fromLocal,
							false };

					if (fromCreated)
					{
						setFlagged();
					}


				}
				auto stat = generateLocalAssign(location, toLocal, toCreated,
					copy(&expr, 1));
				body.push_back(stat);
				break;
			}
			case OpCode::GetGlobal:
			{
				Luau::Parser::Location location = { position, position };
				auto[local, created] =
					findOrCreateLocal(localStack, location, instr.a);

				i++;
				uint32_t constantIndex = p->code[i].encoded;

				auto globalNameConst =
					p->constants[constantIndex]->as<Luau::Parser::AstExprConstantString>();
				std::string globalName{ globalNameConst->value.data,
					globalNameConst->value.size };

				Luau::Parser::AstExpr* globalExpr =
					new (a) Luau::Parser::AstExprGlobal{ location,
						Luau::Parser::AstName{ _strdup(globalName.c_str()) } };

				auto stat = generateLocalAssign(location, local, created,
					copy(&globalExpr, 1));
				body.push_back(stat);

				// TODO: verify hash in arg c

				break;
			}
			case OpCode::SetGlobal:
			{
				Luau::Parser::Location location = { position, position };
				auto[local, created] =
					findOrCreateLocal(localStack, location, instr.a);

				Luau::Parser::AstExpr* valueExpr =
					new (a) Luau::Parser::AstExprLocal{ location, local, false };

				i++;
				uint32_t constantIndex = p->code[i].encoded;
				auto globalNameConst =
					p->constants[constantIndex]->as<Luau::Parser::AstExprConstantString>();
				std::string globalName{ globalNameConst->value.data,
					globalNameConst->value.size };

				Luau::Parser::AstExpr* globalExpr =
					new (a) Luau::Parser::AstExprGlobal{ location,
						Luau::Parser::AstName{ _strdup(globalName.c_str()) } };

				auto stat = new (a) Luau::Parser::AstStatAssign{ location,
					copy(&globalExpr, 1), copy(&valueExpr, 1) };
				body.push_back(stat);
				break;
			}
			case OpCode::GetUpvalue:
			{
				Luau::Parser::Location location = { position, position };
				auto[resLocal, resCreated] =
					findOrCreateLocal(localStack, location, instr.a);

				auto upLocal = p->upvalues.at(instr.b);

				Luau::Parser::AstExpr* expr =
					new (a) Luau::Parser::AstExprLocal{ location, upLocal, true };

				auto stat =
					generateLocalAssign(location, resLocal, resCreated, copy(&expr, 1));
				body.push_back(stat);
				break;
			}
			case OpCode::SetUpvalue:
			{
				Luau::Parser::Location location = { position, position };

				auto[valueLocal, valueCreated] =
					findOrCreateLocal(localStack, location, instr.a);
				auto upLocal = p->upvalues.at(instr.b);

				Luau::Parser::AstExpr* expr =
					new (a) Luau::Parser::AstExprLocal{ location, valueLocal, true };

				auto stat =
					generateLocalAssign(location, upLocal, false, copy(&expr, 1));
				body.push_back(stat);
				break;
			}
			case OpCode::SaveRegisters: break;
			case OpCode::GetGlobalConst:
			{
				Luau::Parser::Location location = { position, position };
				auto[local, created] =
					findOrCreateLocal(localStack, location, instr.a);
				Luau::Parser::AstExpr* expr =
					p->constants[instr.b_x]; // TODO: copy and set location

				auto stat = generateLocalAssign(location, local, created,
					copy(&expr, 1));
				body.push_back(stat);

				i++;
				break;
			}
			case OpCode::GetTableIndex:
			{
				Luau::Parser::Location location = { position, position };

				auto[resLocal, resCreated] =
					findOrCreateLocal(localStack, location, instr.a);
				auto[tableLocal, tableCreated] =
					findOrCreateLocal(localStack, location, instr.b);

				auto[indexLocal, indexCreated] =
					findOrCreateLocal(localStack, location, instr.c);

				if (tableCreated || indexCreated)
					setFlagged();

				auto tableExpr = new (a) Luau::Parser::AstExprLocal{ location,
					tableLocal, false };

				auto indexExpr = new (a) Luau::Parser::AstExprLocal{ location,
					indexLocal, false };

				Luau::Parser::AstExpr* expr =
					new (a) Luau::Parser::AstExprIndexExpr{ location,
						tableExpr, indexExpr };

				auto stat = generateLocalAssign(location, resLocal, resCreated,
					copy(&expr, 1));

				body.push_back(stat);
				break;
			}
			case OpCode::SetTableIndex:
			{
				Luau::Parser::Location location = { position, position };

				auto[valueLocal, valueCreated] =
					findOrCreateLocal(localStack, location, instr.a);

				auto[tableLocal, tableCreated] =
					findOrCreateLocal(localStack, location, instr.b);

				auto[indexLocal, indexCreated] =
					findOrCreateLocal(localStack, location, instr.c);

				Luau::Parser::AstExpr* valueExpr =
					new (a) Luau::Parser::AstExprLocal{ location, valueLocal, false };

				Luau::Parser::AstExpr* tableExpr =
					new (a) Luau::Parser::AstExprLocal{ location, tableLocal, false };

				auto indexExpr = new (a) Luau::Parser::AstExprLocal{ location,
					indexLocal, false };

				Luau::Parser::AstExpr* expr =
					new (a) Luau::Parser::AstExprIndexExpr{ location,
						tableExpr, indexExpr };

				auto stat = new (a) Luau::Parser::AstStatAssign{ location, copy(&expr, 1),
					copy(&valueExpr, 1) };
				body.push_back(stat);

				break;
			}
			case OpCode::GetTableIndexConstant:
			{
				Luau::Parser::Location location = { position, position };

				auto[resLocal, resCreated] =
					findOrCreateLocal(localStack, location, instr.a);
				auto[tableLocal, tableCreated] =
					findOrCreateLocal(localStack, location, instr.b);

				if (tableCreated)
					setFlagged();
				/*
				if (resCreated && !tableCreated && instr.a + 1 == instr.b)
				{
					localStack.erase(instr.b);
				}*/

				i++;
				uint32_t constantIndex = p->code[i].encoded;

				auto tableExpr = new (a) Luau::Parser::AstExprLocal{ location,
					tableLocal, false };

				auto indexExpr = p->constants[constantIndex];

				Luau::Parser::AstExpr* expr =
					new (a) Luau::Parser::AstExprIndexExpr{ location,
						tableExpr, indexExpr };

				auto stat = generateLocalAssign(location, resLocal, resCreated,
					copy(&expr, 1));

				body.push_back(stat);

				// TODO: verify hash
				break;
			}
			case OpCode::SetTableIndexConstant:
			{
				Luau::Parser::Location location = { position, position };

				auto[valueLocal, valueCreated] =
					findOrCreateLocal(localStack, location, instr.a);

				auto[tableLocal, tableCreated] =
					findOrCreateLocal(localStack, location, instr.b);

				Luau::Parser::AstExpr* valueExpr =
					new (a) Luau::Parser::AstExprLocal{ location, valueLocal, false };

				Luau::Parser::AstExpr* tableExpr =
					new (a) Luau::Parser::AstExprLocal{ location, tableLocal, false };

				i++;
				uint32_t constantIndex = p->code[i].encoded;
				auto indexExpr = p->constants[constantIndex];

				Luau::Parser::AstExpr* expr =
					new (a) Luau::Parser::AstExprIndexExpr{ location,
						tableExpr, indexExpr };

				auto stat = new (a) Luau::Parser::AstStatAssign{ location, copy(&expr, 1),
					copy(&valueExpr, 1) };
				body.push_back(stat);

				break;
			}
			case OpCode::GetTableIndexByte:
			{
				Luau::Parser::Location location = { position, position };

				auto[resLocal, resCreated] =
					findOrCreateLocal(localStack, location, instr.a);
				auto[tableLocal, tableCreated] =
					findOrCreateLocal(localStack, location, instr.b);

				if (tableCreated)
					setFlagged();

				auto tableExpr = new (a) Luau::Parser::AstExprLocal{ location,
					tableLocal, false };

				Luau::Parser::AstExpr* indexExpr =
					new (a) Luau::Parser::AstExprConstantNumber{ location, double(instr.c + 1) };

				Luau::Parser::AstExpr* expr =
					new (a) Luau::Parser::AstExprIndexExpr{ location,
						tableExpr, indexExpr };

				auto stat = generateLocalAssign(location, resLocal, resCreated,
					copy(&expr, 1));
				body.push_back(stat);

				break;
			}
			case OpCode::SetTableIndexByte:
			{
				Luau::Parser::Location location = { position, position };

				auto[valueLocal, valueCreated] =
					findOrCreateLocal(localStack, location, instr.a);

				auto[tableLocal, tableCreated] =
					findOrCreateLocal(localStack, location, instr.b);

				/*
				if (localStack.find(instr.a + 1) == localStack.cend())
				{
					localStack.erase(instr.a);
					if (instr.b + 1 == instr.c
						&& instr.c + 1 == instr.a)
					{
						localStack.erase(instr.b);
						localStack.erase(instr.c);
					}
				}
				else if (instr.b + 1 == instr.c
					&& localStack.find(instr.c + 1) == localStack.cend())
				{
					localStack.erase(instr.b);
					localStack.erase(instr.c);
				}*/

				Luau::Parser::AstExpr* valueExpr =
					new (a) Luau::Parser::AstExprLocal{ location, valueLocal, false };

				Luau::Parser::AstExpr* tableExpr =
					new (a) Luau::Parser::AstExprLocal{ location, tableLocal, false };

				Luau::Parser::AstExpr* indexExpr =
					new (a) Luau::Parser::AstExprConstantNumber{ location, double(instr.c + 1) };

				Luau::Parser::AstExpr* expr =
					new (a) Luau::Parser::AstExprIndexExpr{ location,
						tableExpr, indexExpr };

				auto stat = new (a) Luau::Parser::AstStatAssign{ location, copy(&expr, 1),
					copy(&valueExpr, 1) };
				body.push_back(stat);
				break;
			}
			case OpCode::Closure:
			{
				Luau::Parser::Location location = { position, position };

				auto[resLocal, resCreated] =
					findOrCreateLocal(localStack, location, instr.a);

				auto childProto = p->children.at(instr.b_x);

				bool useLocalFunction = false;

				for (byte j = 0; j < childProto->upvalCount; ++j)
				{
					i++;

					auto upInstr = p->code.at(i);
					if (upInstr.op == OpCode::Move)
					{
						auto[upLocal, upCreated] =
							findOrCreateLocal(localStack, location, upInstr.b);
						if (upCreated)
							setFlagged();

						if (upLocal == resLocal)
							useLocalFunction = true;

						childProto->upvalues.push_back(upLocal);
					}
					else if (upInstr.op == OpCode::GetUpvalue)
					{
						childProto->upvalues.push_back(p->upvalues.at(upInstr.b));
					}
					else
					{
						setFlagged();
					}
				}

				auto blockStat = decompile(childProto);
				Luau::Parser::AstExpr* funcExpr =
					new (a) Luau::Parser::AstExprFunction{ location, resLocal,
						copy(childProto->args), childProto->isVarArg != 0,
						{}, blockStat };

				Luau::Parser::AstStat* stat;
				if (useLocalFunction && resCreated)
				{
					stat = new (a) Luau::Parser::AstStatLocalFunction{ location, resLocal,
						funcExpr };
				}
				else
				{
					stat = generateLocalAssign(location, resLocal, resCreated,
						copy(&funcExpr, 1));
				}
				body.push_back(stat);

				break;
			}
			case OpCode::Self:
			{
				Luau::Parser::Location location = { position, position };

				self = true;
				i++;
				uint32_t constantIndex = p->code[i].encoded;

				auto[resLocal, resCreated] = findOrCreateLocal(localStack, location, instr.a);
				auto[tableLocal, tableCreated] = findOrCreateLocal(localStack, location, instr.b);
				
				auto tableExpr = new (a) Luau::Parser::AstExprLocal{ location,
					tableLocal, false };
				auto indexExpr = p->constants[constantIndex];

				auto nameString = indexExpr->as<Luau::Parser::AstExprConstantString>()->value;
				char* nameData = new (a) char[nameString.size + 1];
				memcpy(nameData, nameString.data, nameString.size);
				nameData[nameString.size] = '\0';

				auto indexName = Luau::Parser::AstName{ nameData };

				Luau::Parser::AstExpr* expr =
					new (a) Luau::Parser::AstExprIndexName{ location,
						tableExpr, indexName, location };
				/*
				auto stat = generateLocalAssign(location, resLocal, resCreated,
					copy(&expr, 1));

				body.push_back(stat);*/

				selfExpr = expr;
				break;
			}
			case OpCode::Call:
			{
				// if (!instr.b)
				// 	throw std::runtime_error("this operation is currently unsupported");

				Luau::Parser::Location location = { position, position };

				auto callBaseReg = instr.a;

				Luau::Parser::AstExpr* funcExpr;
				if (self)
				{
					funcExpr = selfExpr;
				}
				else
				{
					auto funcLocal = localStack.at(callBaseReg);
					funcExpr = new (a) Luau::Parser::AstExprLocal{ location,
						funcLocal,
						funcLocal->functionDepth != functionStack.size() };
				}

				localStack.erase(callBaseReg);

				TempVector<Luau::Parser::AstExpr*> args{ scratchExpr };
				if (instr.b)
				{
					for (byte j = 1 + self; j < instr.b; j++)
					{
						auto local = localStack.at(callBaseReg + j);
						args.push_back(new (a) Luau::Parser::AstExprLocal{ location,
							local, local->functionDepth != functionStack.size() });
						localStack.erase(callBaseReg + j);
					}
				}
				else
				{
					for (byte j = callBaseReg + 1 + self; j < tailBase; j++)
					{
						auto local = localStack.at(j);
						args.push_back(new (a) Luau::Parser::AstExprLocal{ location,
							local, local->functionDepth != functionStack.size() });
						localStack.erase(j);
					}

					args.push_back(tailExpr);
				}

				Luau::Parser::AstExpr* expr =
					new (a) Luau::Parser::AstExprCall{ location, funcExpr,
					copy(args), self }; // TODO: self

				if (instr.c)
				{
					if (instr.c - 1 != 0)
					{
						TempVector<Luau::Parser::AstLocal*> locals{ scratchLocal };
						for (byte j = 0; j < instr.c - 1; j++)
						{
							auto[local, created] =
								findOrCreateLocal(localStack, location, callBaseReg + j);
							locals.push_back(local);
						}

						auto stat = new (a) Luau::Parser::AstStatLocal{ location,
							copy(locals), copy(&expr, 1) };
						body.push_back(stat);
					}
					else
					{
						auto stat = new (a) Luau::Parser::AstStatExpr{ location, expr };
						body.push_back(stat);
					}
				}
				else // tail call
				{
					isTail = true;
					tailExpr = expr;
					tailBase = callBaseReg;
				}

				self = false;

				break;
			}
			case OpCode::Return:
			{
				if (instr.b == 1
					&& (p->isMain || i == p->code.size() - 1))
				{
					break;
				}

				Luau::Parser::Location location = { position, position };

				TempVector<Luau::Parser::AstExpr*> values{ scratchExpr };
				if (instr.b == 0)
				{
					if (!isTail)
						throw std::runtime_error("expected tail expression.");

					for (byte j = instr.a; j < tailBase; ++j)
					{
						auto local = localStack.at(j);
						values.push_back(new (a) Luau::Parser::AstExprLocal{ location,
							local, local->functionDepth != functionStack.size() });
						localStack.erase(j);
					}

					values.push_back(tailExpr);

					isTail = false;
				}
				else
				{
					for (byte j = 0; j < instr.b - 1; j++)
					{
						auto local = localStack.at(instr.a + j);
						values.push_back(new (a) Luau::Parser::AstExprLocal{ location,
							local, local->functionDepth != functionStack.size() });
						localStack.erase(instr.a + j);
					}
				}

				auto stat = new (a) Luau::Parser::AstStatReturn{ location,
					copy(values) };

				body.push_back(stat);

				break;
			}
			case OpCode::Jump:
			{
				std::cout << "unsupported opcode jump\n";
				break;
			}
			case OpCode::LoopJump:
			{
				// loop jumps always jump backwards (iirc)
				if (i + instr.s_b_x >= body.size())
				{
					setFlagged();
					std::cout << "what\n";
				}

				bool repeat = false;

				Luau::Parser::Location location = { position, position };

				auto bodyStartIndex = instrBodyMap.at(i + instr.s_b_x);

				Luau::Parser::AstExpr* condExpr = new (a) Luau::Parser::AstExprConstantBool{ location, true };

				if (!f.empty()
					&& i == f.front().codeEndIndex)
				{
					auto cfInfo = f.front();
					if (cfInfo.codeStartIndex == i - 1)
					{
						repeat = true;
					}

					condExpr = new (a) Luau::Parser::AstExprLocal{ cfInfo.location, cfInfo.local, false };
					bodyStartIndex = cfInfo.bodyStartIndex;
					f.pop_front();
				}

				std::vector<Luau::Parser::AstStat*> innerBody{};
				std::copy(body.begin() + bodyStartIndex, body.end(),
					std::back_inserter(innerBody));

				body.resize(bodyStartIndex);

				optimize(innerBody);

				auto blockStat = new (a) Luau::Parser::AstStatBlock{ location, copy(innerBody) };

				auto whileStat = new (a) Luau::Parser::AstStatWhile{ location,
					condExpr, blockStat };

				body.push_back(whileStat);

				break;
			}
			case OpCode::Test:
			case OpCode::NotTest:
			{
				Luau::Parser::Location location = { position, position };

				auto[local, created] = findOrCreateLocal(localStack, location, instr.a);
				if (created)
					setFlagged();

				if (i + instr.s_b_x <= body.size())
				{
					setFlagged();
					std::cout << "what\n";
				}

				f.push_back({ i, body.size(), i + instr.s_b_x, local,
					ControlFlowInfo::Type(byte(instr.op) - byte(OpCode::Test)), location });
				break;
			}
			case OpCode::Equal:
			case OpCode::LesserOrEqual:
			case OpCode::LesserThan:
			case OpCode::NotEqual:
			case OpCode::GreaterThan:
			case OpCode::GreaterOrEqual:
			{
				i++;
				break;
			}
			case OpCode::Add:
			case OpCode::Sub:
			case OpCode::Mul:
			case OpCode::Div:
			case OpCode::Mod:
			case OpCode::Pow:
			{
				Luau::Parser::Location location{ position, position };

				auto[leftLocal, leftCreated] =
					findOrCreateLocal(localStack, location, instr.b);
				if (leftCreated)
					setFlagged();

				auto[rightLocal, rightCreated] =
					findOrCreateLocal(localStack, location, instr.c);
				if (rightCreated)
					setFlagged();
				/*
				if (instr.c == instr.b + 1 && instr.a == instr.b)
				{
					localStack.erase(instr.b);
					localStack.erase(instr.c);
				}*/
				auto[resLocal, resCreated] =
					findOrCreateLocal(localStack, location, instr.a);

				auto leftExpr = new (a) Luau::Parser::AstExprLocal{ location,
					leftLocal, false };

				auto rightExpr = new (a) Luau::Parser::AstExprLocal{ location,
					rightLocal, false };

				auto binaryOp =
					Luau::Parser::AstExprBinary::Op(byte(instr.op) - byte(OpCode::Add));

				Luau::Parser::AstExpr* expr =
					new (a) Luau::Parser::AstExprBinary(location, binaryOp,
						leftExpr, rightExpr);

				auto stat = generateLocalAssign(location, resLocal, resCreated,
					copy(&expr, 1));
				body.push_back(stat);

				break;
			}
			case OpCode::AddByte:
			case OpCode::SubByte:
			case OpCode::MulByte:
			case OpCode::DivByte:
			case OpCode::ModByte:
			case OpCode::PowByte:
			{
				Luau::Parser::Location location{ position, position };

				auto[leftLocal, leftCreated] =
					findOrCreateLocal(localStack, location, instr.b);
				if (leftCreated)
					setFlagged();

				auto rightConstIndex = instr.c;
				/*
				if (instr.a == instr.b + 1)
				{
					localStack.erase(instr.b);
				}*/
				auto[resLocal, resCreated] =
					findOrCreateLocal(localStack, location, instr.a);

				auto leftExpr = new (a) Luau::Parser::AstExprLocal{ location,
					leftLocal, false };

				auto rightExpr = p->constants.at(rightConstIndex);

				auto binaryOp =
					Luau::Parser::AstExprBinary::Op(byte(instr.op) - byte(OpCode::AddByte));

				Luau::Parser::AstExpr* expr =
					new (a) Luau::Parser::AstExprBinary(location, binaryOp,
						leftExpr, rightExpr);

				auto stat = generateLocalAssign(location, resLocal, resCreated,
					copy(&expr, 1));
				body.push_back(stat);

				break;
			}
			case OpCode::Or:
				std::cout << "unsupported opcode or\n";
				break;
			case OpCode::And:
				std::cout << "unsupported opcode and\n";
				break;
			case OpCode::OrByte:
				std::cout << "unsupported opcode orbyte\n";
				break;
			case OpCode::AndByte:
				std::cout << "unsupported opcode andbyte\n";
				break;
			case OpCode::Concat:
			{
				Luau::Parser::Location location{ position, position };

				auto[resLocal, resCreated] =
					findOrCreateLocal(localStack, location, instr.a);

				auto[startLocal, startCreated] =
					findOrCreateLocal(localStack, location, instr.b);

				if (startCreated)
					setFlagged();

				Luau::Parser::AstExpr* expr =
					new (a) Luau::Parser::AstExprLocal{ location, startLocal, false };
				for (byte j = instr.b + 1; j <= instr.c; ++j)
				{
					auto[rhsLocal, rhsCreated] = findOrCreateLocal(localStack, location, j);
					if (rhsCreated)
						setFlagged();

					auto rhsExpr =
						new (a) Luau::Parser::AstExprLocal{ location, rhsLocal, false };

					expr = new (a) Luau::Parser::AstExprBinary{ location,
						Luau::Parser::AstExprBinary::Concat, expr, rhsExpr };
				}

				auto stat =
					generateLocalAssign(location, resLocal, resCreated, copy(&expr, 1));

				body.push_back(stat);

				break;
			}
			case OpCode::Not:
			case OpCode::UnaryMinus:
			case OpCode::Len:
			{
				Luau::Parser::Location location{ position, position };

				auto[resLocal, resCreated] =
					findOrCreateLocal(localStack, location, instr.a);

				auto[operandLocal, operandCreated] =
					findOrCreateLocal(localStack, location, instr.b);

				if (operandCreated)
					setFlagged();

				auto unaryOp =
					Luau::Parser::AstExprUnary::Op(byte(instr.op) - byte(OpCode::Not));

				Luau::Parser::AstExpr* operandExpr =
					new (a) Luau::Parser::AstExprLocal{ location, operandLocal, false };

				Luau::Parser::AstExpr* expr =
					new (a) Luau::Parser::AstExprUnary{ location, unaryOp, operandExpr };

				auto stat =
					generateLocalAssign(location, resLocal, resCreated, copy(&expr, 1));
				body.push_back(stat);

				break;
			}
			case OpCode::NewTable:
				i++;
			case OpCode::NewTableConst:
			{
				Luau::Parser::Location location{ position, position };

				auto[resLocal, resCreated] =
					findOrCreateLocal(localStack, location, instr.a);

				Luau::Parser::AstExpr* expr =
					new (a) Luau::Parser::AstExprTable{ location, {} };

				auto stat =
					generateLocalAssign(location, resLocal, resCreated, copy(&expr, 1));
				body.push_back(stat);
				break;
			}
			case OpCode::SetList:
				i++;
				break;
			case OpCode::ForPrep:
				std::cout << "unsupported opcode forprep\n"; 
				break;
			case OpCode::ForLoop:
				std::cout << "unsupported opcode forloop\n";
				break;
			case OpCode::TForLoop:
				std::cout << "unsupported opcode tforloop\n";
				break;
			case OpCode::LoopJumpIPairs:
				std::cout << "unsupported opcode loopjumpipairs\n";
				break;
			case OpCode::TForLoopIPairs:
				std::cout << "unsupported opcode tforloopipairs\n";
				break;
			case OpCode::LoopJumpNext:
				std::cout << "unsupported opcode loopjumpnext\n";
				break;
			case OpCode::TForLoopNext:
				std::cout << "unsupported opcode tforloopnext\n";
				break;
			case OpCode::LoadVarargs:
			{
				Luau::Parser::Location location{ position, position };

				Luau::Parser::AstExpr* vaExpr =
					new (a) Luau::Parser::AstExprVarargs{ location };

				if (instr.b == 0)
				{
					isTail = true;
					tailBase = instr.a;
					tailExpr = vaExpr;
					break;
				}

				bool last = false;
				TempVector<Luau::Parser::AstLocal*> locals{ scratchLocal };
				for (size_t j = 0; j < instr.b - 1; ++j)
				{
					auto[local, created] =
						findOrCreateLocal(localStack, location, instr.a + j);
					if (j != 0 && created != last)
						throw std::runtime_error("unexpected error (ldva).");
					locals.push_back(local);
					last = created;
				}

				Luau::Parser::AstStat* stat;
				if (last)
				{
					stat = new (a) Luau::Parser::AstStatLocal{ location,
						copy(locals), copy(&vaExpr, 1) };
				}
				else
				{
					throw std::runtime_error("what the fuck.");
				}
				body.push_back(stat);
				
				break;
			}
			case OpCode::ClearStack: break;
			case OpCode::ClearStackFull: break;
			case OpCode::LoadConstLarge:
				std::cout << "unsupported opcode load const large (damn that's a large script)\n";
				break;
			case OpCode::FarJump:
				std::cout << "unsupported opcode farjump\n";
				break;
			case OpCode::BuiltinCall:
				std::cout << "unsupported opcode builtincall\n";
				break;
			default:;
			}

			if (!f.empty()
				&& i == f.front().codeEndIndex)
			{
				auto cfInfo = f.front();

				auto location = cfInfo.location;

				std::vector<Luau::Parser::AstStat*> innerBody{};
				if (body.begin() + cfInfo.bodyStartIndex <= body.end())
				{
					std::copy(body.begin() + cfInfo.bodyStartIndex, body.end(),
						std::back_inserter(innerBody));

					body.resize(f.front().bodyStartIndex);
				}
				optimize(innerBody);

				auto bodyArray = copy<Luau::Parser::AstStat*>(innerBody);
				auto bodyStat = new (a) Luau::Parser::AstStatBlock{ location, bodyArray };

				Luau::Parser::AstExpr* condExpr = new (a) Luau::Parser::AstExprLocal{ location, cfInfo.local,
					false };

				if (cfInfo.type == ControlFlowInfo::Type::Test)
				{
					condExpr = new (a) Luau::Parser::AstExprUnary{ location, Luau::Parser::AstExprUnary::Op::Not,
						condExpr };
				}

				auto stat = new (a) Luau::Parser::AstStatIf{ location, condExpr, bodyStat,
					nullptr };
				body.push_back(stat);

				f.pop_front();
			}
		}

		Luau::Parser::Position start{ p->lineInfo.front(), 0 };
		Luau::Parser::Position end{ p->lineInfo.back(), 0 };

		optimize(body);
		auto bodyArray = copy<Luau::Parser::AstStat*>(body);

		Luau::Parser::Location location{ start, end };
		return new (a) Luau::Parser::AstStatBlock{ location, bodyArray };
	}

	void optimize(/*TempVector*/std::vector<Luau::Parser::AstStat*>& body)
	{
		LocalCollector localCollector{};
		for (const auto& stat : body)
		{
			stat->visit(&localCollector);
		}

		auto& localInfo = localCollector.localInfo;

		// split locals
		std::vector<Luau::Parser::AstStatAssign*> toSplit{};
		std::vector<LocalInliner> inliners;
		for (auto& stat : body)
		{
			for (auto inl : inliners)
			{
				stat->visit(&inl);
			}

			if (auto assignStat = stat->as<Luau::Parser::AstStatAssign>())
			{
				auto it = std::find(toSplit.cbegin(), toSplit.cend(), assignStat);
				if (it != toSplit.cend())
				{
					Luau::Parser::AstLocal* local = assignStat->vars.data[0]->as<Luau::Parser::AstExprLocal>()->local;
					Luau::Parser::AstLocal* newLocal = createLocal(assignStat->location);
					inliners.emplace_back(local, new (a) Luau::Parser::AstExprLocal{ assignStat->location, newLocal, false });
					stat = new (a) Luau::Parser::AstStatLocal{ assignStat->location,
						copy(&newLocal, 1), assignStat->values };
				}

				continue;
			}

			auto localStat = stat->as<Luau::Parser::AstStatLocal>();
			if (!localStat || localStat->vars.size > 1)
				continue;

			auto local = localStat->vars.data[0];
			auto it = localInfo.find(local);
			if (it == localInfo.cend())
				continue;

			auto info = it->second;
			if (info.references.size() <= 1)
				continue;

			bool lastAssign = false;
			for (auto ref : info.references)
			{
				auto assignStatRef = ref->as<Luau::Parser::AstStatAssign>();
				auto localStatRef = ref->as<Luau::Parser::AstStatLocal>();
				if (lastAssign && assignStatRef)
				{
					for (auto var : assignStatRef->vars)
					{
						auto localExpr = var->as<Luau::Parser::AstExprLocal>();
						if (!localExpr)
							continue;

						if (localExpr->local == local)
						{
#ifdef _DEBUG
							printf("local '%s' can be split\n", local->name.value);
#endif
							toSplit.push_back(assignStatRef);
							break;
						}
					}

					lastAssign = false;
				}
				else if (assignStatRef)
				{
					lastAssign = true;
					for (auto var : assignStatRef->vars)
					{
						auto localExpr = var->as<Luau::Parser::AstExprLocal>();
						if (!localExpr)
							continue;

						if (localExpr->local == local)
						{
							lastAssign = false;
						}
					}
				}
				else if (localStatRef)
				{
					lastAssign = true;
				}
				else
				{
					lastAssign = false;
				}
			}
		}

		localInfo.clear();

		for (const auto& stat : body)
		{
			stat->visit(&localCollector);
		}


		// Optimize single reference locals.
		auto end = std::remove_if(body.begin(), body.end(),
		[&](Luau::Parser::AstStat* stat)
		{
			if (auto localStat = stat->as<Luau::Parser::AstStatLocal>())
			{
				size_t optimized = 0;
				auto lastVal = localStat->values.data[localStat->values.size - 1];
				bool isTailRes = lastVal->is<Luau::Parser::AstExprVarargs>()
					|| lastVal->is<Luau::Parser::AstExprCall>();
				if (isTailRes && localStat->vars.size > 1)
					return false;
				for (size_t i = 0; i < localStat->vars.size; ++i)
				{
					auto local = localStat->vars.data[i];
					auto infoIt = localInfo.find(local);

					if (infoIt == localInfo.cend())
						continue;
					auto info = infoIt->second;

					if (info.references.size() == 1)
					{
						auto refStat = info.references.at(0);
						if (auto assignStat = refStat->as<Luau::Parser::AstStatAssign>())
						{
							for (const auto& varExpr : assignStat->vars)
							{
								if (auto localExpr
									= varExpr->as<Luau::Parser::AstExprLocal>())
								{
									if (localExpr->local == local)
										goto opt_fail;
								}
							}
						}
#ifdef _DEBUG
						printf("optimizable local (%s) discovered!\n",
							local->name.value);
#endif

						LocalInliner localInliner{ local, localStat->values.data[i] };
						refStat->visit(&localInliner);

						/*std::string nameString = "var";

						if (localStat->values.data[i]->constEval() == Luau::Parser::ConstEvalResult::True) {
							if (localStat->values.data[i]->is< Luau::Parser::AstExprConstantString>()) {
								nameString = "text";
							}
						}

						nameString += std::to_string(c++);

						char* nameData = new (a) char[nameString.length() + 1];
						memcpy(nameData, nameString.c_str(), nameString.length() + 1);
						auto name = Luau::Parser::AstName{ nameData };
						local->name = name;
						localStat->vars.data[i] = local;*/

						optimized++;
					}
				opt_fail: {}
				}

				if (optimized == localStat->vars.size)
				{
					return true;
				}
			}
			return false;
		});

		// TODO: Smart Optimization
		body.erase(end, body.end());
		//body.erase(body.begin(), end);
	}
public:
	Decompiler(Luau::Parser::Allocator& a/*, Luau::Parser::AstNameTable& names*/)
		: a(a) /*, names(names)*/ {}

	bool wasFlagged()
	{
		return flagged;
	}

	Luau::Parser::AstStat* operator()(const std::vector<byte>& bytecode)
	{
		flagged = false;
		generateOpConvTable();

		std::cout << "co1";

		BytecodeReader reader{ bytecode };

		std::cout << "co2";

		auto success = reader.read<byte>();
		std::cout << "co3";
		if (success > 1)
			throw std::runtime_error("bytecode version mismatch");
		if (success == 0) // TODO: test
			throw std::runtime_error(
				std::string{ (const char*)(bytecode.data() + 1), bytecode.size() - 1 });
		std::cout << "co4";
		auto stringCount = reader.readInt();
		std::cout << "co5";
		stringTable.reserve(stringCount);
		for (int i = 0; i < stringCount; ++i)
		{
			auto stringSize = reader.readInt();
			auto string = reader.read<char>(stringSize);
			stringTable.emplace_back(string, stringSize);
		}

		auto protoCount = reader.readInt();
		protos.reserve(protoCount);
		for (int i = 0; i < protoCount; ++i)
		{
			auto p = new (a) Proto{};
			p->maxRegCount = reader.read<byte>();
			p->argCount = reader.read<byte>();
			p->upvalCount = reader.read<byte>();
			p->isVarArg = reader.read<byte>();

			bool studio = false;

			auto instrCount = reader.readInt();
			p->code.reserve(instrCount);
			for (auto j = 0; j < instrCount; ++j)
			{
				auto instr = reader.read<Instruction>();
				if (j == 0 && instr.op == OpCode::ClearStackFull)
				{
					studio = true;
				}
				if (!studio)
					instr.op = opConversionTable.at(byte(instr.op));
				p->code.push_back(instr);

				switch (instr.op)
				{
				case OpCode::GetGlobal:
				case OpCode::SetGlobal:
				case OpCode::GetGlobalConst:
				case OpCode::GetTableIndexConstant:
				case OpCode::SetTableIndexConstant:
				case OpCode::Self:
				case OpCode::Equal:
				case OpCode::LesserOrEqual:
				case OpCode::LesserThan:
				case OpCode::NotEqual:
				case OpCode::GreaterThan:
				case OpCode::GreaterOrEqual:
				case OpCode::NewTable:
				case OpCode::SetList:
				case OpCode::TForLoop:
				case OpCode::LoadConstLarge:
					++j;
					p->code.push_back(reader.read<Instruction>());
				default:;
				}
			}

			auto constCount = reader.readInt();
			p->constants.reserve(constCount);
			for (auto j = 0; j < constCount; ++j)
			{
				Luau::Parser::Position position{ 0, 0 };
				Luau::Parser::Location location{ position, position };
				Luau::Parser::AstExpr* expr;

				switch (reader.read<ConstantType>())
				{
				case ConstantType::ConstantNil:
				{
					setFlagged();
					expr = new (a) Luau::Parser::AstExprConstantNil{ location };
					break;
				}
				case ConstantType::ConstantBoolean:
				{
					setFlagged();
					expr = new (a) Luau::Parser::AstExprConstantBool{ location,
						reader.read<bool>() };
					break;
				}
				case ConstantType::ConstantNumber:
				{
					expr = new (a) Luau::Parser::AstExprConstantNumber{ location,
						reader.read<double>() };
					break;
				}
				case ConstantType::ConstantString:
				{
					auto strVal = stringTable.at(reader.readInt() - 1);
					Luau::Parser::AstArray<char> strData{};
					char* nameData = new (a) char[strVal.length()];
					memcpy(nameData, strVal.data(), strVal.length());
					strData.data = nameData;
					strData.size = strVal.length();

					expr = new (a) Luau::Parser::AstExprConstantString{ location,
						strData };
					break;
				}
				case ConstantType::ConstantGlobal:
				{
					auto encodedIndicies = reader.read<uint32_t>();
					int index1;
					int index2;
					int index3;

					uint32_t v5 = encodedIndicies >> 30;
					if (encodedIndicies >> 30)
						index1 = (encodedIndicies >> 20) & 0x3FF;
					else
						index1 = -1;
					if (v5 <= 1)
						index2 = -1;
					else
						index2 = (encodedIndicies >> 10) & 0x3FF;
					index3 = -1;
					if (v5 > 2)
						index3 = encodedIndicies & 0x3FF;

					auto nameString1 = p->constants.at(index1)->as<Luau::Parser::AstExprConstantString>()->value;
					char* nameData1 = new (a) char[nameString1.size + 1];
					memcpy(nameData1, nameString1.data, nameString1.size);
					nameData1[nameString1.size] = '\0';

					auto name1 = Luau::Parser::AstName{ nameData1 };
					expr = new (a) Luau::Parser::AstExprGlobal{ location, name1 };

					if (index2 >= 0)
					{
						auto nameString2 = p->constants.at(index2)->as<Luau::Parser::AstExprConstantString>()->value;
						char* nameData2 = new (a) char[nameString2.size + 1];
						memcpy(nameData2, nameString2.data, nameString2.size);
						nameData2[nameString2.size] = '\0';

						auto name2 = Luau::Parser::AstName{ nameData2 };
						expr = new (a) Luau::Parser::AstExprIndexName{ location, expr,
							name2, location };
					}

					if (index3 >= 0)
					{
						auto nameString3 = p->constants.at(index3)->as<Luau::Parser::AstExprConstantString>()->value;
						char* nameData3 = new (a) char[nameString3.size + 1];
						memcpy(nameData3, nameString3.data, nameString3.size);
						nameData3[nameString3.size] = '\0';

						auto name3 = Luau::Parser::AstName{ nameData3 };
						expr = new (a) Luau::Parser::AstExprIndexName{ location, expr,
							name3, location };
					}
					break;
				}
				case ConstantType::ConstantHashTable:
				{
					// throw std::runtime_error("unsupported constant type 'HashTable'");
					auto hashSize = reader.readInt();
					for (int j = 0; j < hashSize; ++j)
					{
						reader.readInt();
					}
					break;
				}
				default:
					throw std::runtime_error("unsupported constant type");
				}

				p->constants.push_back(expr);
			}

			auto closureCount = reader.readInt();
			p->children.reserve(closureCount);
			for (auto j = 0; j < closureCount; ++j)
			{
				p->children.push_back(protos.at(reader.readInt()));
			}

			auto nameIndex = reader.readInt();
			if (nameIndex)
			{
				p->name = stringTable.at(nameIndex - 1);
			}

			auto lineInfoCount = reader.readInt();
			p->lineInfo.reserve(lineInfoCount);
			int lastLine = 0;
			for (auto j = 0; j < lineInfoCount; ++j)
			{
				lastLine += reader.readInt();
				p->lineInfo.push_back(lastLine);
			}

			if (lastLine < 0)
				setFlagged();

			if (reader.read<byte>())
				setFlagged();

			protos.push_back(p);
		}

		mainProto = protos.at(reader.readInt());
		mainProto->isMain = true;

		return decompile(mainProto);
	}
};

void Luau::decompile(std::ostream& buff, const std::vector<byte>& bytecode)
{
	try
	{
		Parser::Allocator a;
		// Parser::AstNameTable names{ a };
		Decompiler decompiler{ a/*, names*/ };
		auto root = decompiler(bytecode);

		if (decompiler.wasFlagged())
		{
			buff
				<<
				"--[[\n"
				"\tinput function was flagged as potentially incompatible.\n"
				"\tplease private message a developer for support.\n"
				"]]\n";
		}

		formatAst(buff, root);
	}
	catch (...)
	{
		std::rethrow_exception(std::current_exception());
	}
}
