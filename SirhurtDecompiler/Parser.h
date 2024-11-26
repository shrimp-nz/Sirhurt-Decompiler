#pragma once
#include "TextFormat.h"

#include <string>
#include <cassert>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <map>

#include "parallel_hashmap/phmap.h"

namespace Luau::Parser
{
	class Allocator
	{
	public:
		Allocator()
			: root(static_cast<Page*>(operator new(sizeof(Page))))
			, offset(0)
		{
			root->next = NULL;
		}

		~Allocator()
		{
			Page* page = root;

			while (page)
			{
				Page* next = page->next;

				operator delete(page);

				page = next;
			}
		}

		void* allocate(size_t size)
		{
			// pointer-align all allocations
			size = (size + sizeof(void*) - 1) & ~(sizeof(void*) - 1);

			if (offset + size <= sizeof(root->data))
			{
				void* result = root->data + offset;
				offset += size;
				return result;
			}

			// allocate new page
			void* pageData = operator new(((::size_t)&reinterpret_cast<char const volatile&>((((Page*)0)->data))) + std::max(sizeof(root->data), size));

			Page* page = static_cast<Page*>(pageData);

			page->next = root;

			root = page;
			offset = size;

			return page->data;
		}

	private:
		struct Page
		{
			Page* next;

			char data[8192];
		};

		Page* root;
		unsigned int offset;
	};
}

inline void* operator new(size_t size, Luau::Parser::Allocator& alloc)
{
	return alloc.allocate(size);
}

inline void* operator new[](size_t size, Luau::Parser::Allocator& alloc)
{
	return alloc.allocate(size);
}

inline void operator delete(void*, Luau::Parser::Allocator& alloc)
{
}

inline void operator delete[](void*, Luau::Parser::Allocator& alloc)
{
}

struct CharArrayHasher
{
	size_t operator()(const char *s) const noexcept
	{
		// http://www.cse.yorku.ca/~oz/hash.html
		size_t h = 5381;
		int c;
		while ((c = *s++))
			h = ((h << 5) + h) + c;
		return h;
	}
};

namespace Luau::Parser
{
	struct Position
	{
		unsigned int line, column;

		Position(unsigned int line, unsigned int column)
			: line(line)
			, column(column)
		{
		}
	};

	struct Location
	{
		Position begin, end;

		Location()
			: begin(0, 0)
			, end(0, 0)
		{
		}

		Location(const Position& begin, const Position& end)
			: begin(begin)
			, end(end)
		{
		}

		Location(const Position& begin, unsigned int length)
			: begin(begin)
			, end(begin.line, begin.column + length)
		{
		}

		Location(const Location& begin, const Location& end)
			: begin(begin.begin)
			, end(end.end)
		{
		}
	};

	class ParseError : public std::exception
	{
	public:
		ParseError(const Location& location, const char* format, ...) RBX_PRINTF_ATTR(3, 4)
			: location(location)
		{
			va_list args;
			va_start(args, format);
			message = TextFormat::vformat(format, args);
			va_end(args);
		}

		virtual ~ParseError() throw()
		{
		}

		virtual const char* what() const throw()
		{
			return message.c_str();
		}

		const Location& getLocation() const
		{
			return location;
		}

	private:
		std::string message;
		Location location;
	};

	struct AstName
	{
		const char* value;

		AstName()
			: value(NULL)
		{
		}

		explicit AstName(const char* value)
			: value(value)
		{
		}

		bool operator==(const AstName& rhs) const
		{
			return value == rhs.value;
		}

		bool operator!=(const AstName& rhs) const
		{
			return value != rhs.value;
		}

		bool operator==(const char* rhs) const
		{
			return strcmp(value, rhs) == 0;
		}

		bool operator!=(const char* rhs) const
		{
			return strcmp(value, rhs) != 0;
		}
	};

	template <typename T> struct AstRtti
	{
		static_assert(std::is_class<T>::value);

		static const int value;
	};

	inline int gAstRttiIndex = 0;

	template <typename T> const int AstRtti<T>::value = ++gAstRttiIndex;

#define ASTRTTI(Class) virtual int getClassIndex() const { return AstRtti<Class>::value; }

	template <typename T> struct AstArray
	{
		class iterator {
		public:
			iterator(T * ptr) : ptr(ptr) {}
			iterator operator++() { ++ptr; return *this; }
			bool operator!=(const iterator & other) const { return ptr != other.ptr; }
			const T& operator*() const { return *ptr; }
		private:
			T* ptr;
		};

		T* data;
		size_t size;

		bool operator==(const AstArray<T> rhs)
		{
			return memcmp(data, rhs.data, size) == 0;
		}

		bool operator!=(const AstArray<T> rhs)
		{
			return memcmp(data, rhs.data, size) != 0;
		}

		iterator begin()
		{
			return iterator(data);
		}

		iterator end()
		{
			return iterator(data + size);
		}
	};

	struct AstLocal
	{
		AstName name;
		Location location;
		AstLocal* shadow;
		unsigned int functionDepth;
		bool utilized = false;

		AstLocal(const AstName& name, const Location& location, AstLocal* shadow, unsigned int functionDepth)
			: name(name)
			, location(location)
			, shadow(shadow)
			, functionDepth(functionDepth)
		{
		}
	};

	class AstVisitor
	{
	public:
		virtual ~AstVisitor() {}

		virtual bool visit(class AstExpr* node) { return true; }

		virtual bool visit(class AstExprGroup* node) { return visit((class AstExpr*)node); }
		virtual bool visit(class AstExprConstantNil* node) { return visit((class AstExpr*)node); }
		virtual bool visit(class AstExprConstantBool* node) { return visit((class AstExpr*)node); }
		virtual bool visit(class AstExprConstantNumber* node) { return visit((class AstExpr*)node); }
		virtual bool visit(class AstExprConstantString* node) { return visit((class AstExpr*)node); }
		virtual bool visit(class AstExprLocal* node) { return visit((class AstExpr*)node); }
		virtual bool visit(class AstExprGlobal* node) { return visit((class AstExpr*)node); }
		virtual bool visit(class AstExprVarargs* node) { return visit((class AstExpr*)node); }
		virtual bool visit(class AstExprCall* node) { return visit((class AstExpr*)node); }
		virtual bool visit(class AstExprIndexName* node) { return visit((class AstExpr*)node); }
		virtual bool visit(class AstExprIndexExpr* node) { return visit((class AstExpr*)node); }
		virtual bool visit(class AstExprFunction* node) { return visit((class AstExpr*)node); }
		virtual bool visit(class AstExprTable* node) { return visit((class AstExpr*)node); }
		virtual bool visit(class AstExprUnary* node) { return visit((class AstExpr*)node); }
		virtual bool visit(class AstExprBinary* node) { return visit((class AstExpr*)node); }

		virtual bool visit(class AstStat* node) { return true; }

		virtual bool visit(class AstStatBlock* node) { return visit((class AstStat*)node); }
		virtual bool visit(class AstStatIf* node) { return visit((class AstStat*)node); }
		virtual bool visit(class AstStatWhile* node) { return visit((class AstStat*)node); }
		virtual bool visit(class AstStatRepeat* node) { return visit((class AstStat*)node); }
		virtual bool visit(class AstStatBreak* node) { return visit((class AstStat*)node); }
		virtual bool visit(class AstStatReturn* node) { return visit((class AstStat*)node); }
		virtual bool visit(class AstStatExpr* node) { return visit((class AstStat*)node); }
		virtual bool visit(class AstStatLocal* node) { return visit((class AstStat*)node); }
		virtual bool visit(class AstStatLocalFunction* node) { return visit((class AstStat*)node); }
		virtual bool visit(class AstStatFor* node) { return visit((class AstStat*)node); }
		virtual bool visit(class AstStatForIn* node) { return visit((class AstStat*)node); }
		virtual bool visit(class AstStatAssign* node) { return visit((class AstStat*)node); }
		virtual bool visit(class AstStatFunction* node) { return visit((class AstStat*)node); }
	};

	class AstNode
	{
	public:
		explicit AstNode(const Location& location) : location(location) {}
		virtual ~AstNode() {}

		virtual void visit(AstVisitor* visitor) = 0;

		virtual int getClassIndex() const = 0;

		template <typename T> bool is() const { return getClassIndex() == AstRtti<T>::value; }
		template <typename T> T* as() { return getClassIndex() == AstRtti<T>::value ? static_cast<T*>(this) : nullptr; }

		Location location;
	};

	enum class ConstEvalResult : unsigned char
	{
		False,
		True,
		Invalid
	};

	class AstExpr : public AstNode
	{
	public:
		explicit AstExpr(const Location& location) : AstNode(location) {}

		virtual ConstEvalResult constEval()
		{
			return ConstEvalResult::Invalid;
		}
	};

	class AstStat : public AstNode
	{
	public:
		explicit AstStat(const Location& location) : AstNode(location) {}
	};

	class AstExprGroup : public AstExpr
	{
	public:
		ASTRTTI(AstExprGroup)

			explicit AstExprGroup(const Location& location, AstExpr* expr)
			: AstExpr(location)
			, expr(expr)
		{
		}

		virtual void visit(AstVisitor* visitor)
		{
			if (visitor->visit(this))
				expr->visit(visitor);
		}

		AstExpr* expr;
	};

	class AstExprConstantNil : public AstExpr
	{
	public:
		ASTRTTI(AstExprConstantNil)

			explicit AstExprConstantNil(const Location& location)
			: AstExpr(location)
		{
		}

		virtual void visit(AstVisitor* visitor)
		{
			visitor->visit(this);
		}

		virtual ConstEvalResult constEval()
		{
			return ConstEvalResult::False;
		}
	};

	class AstExprConstantBool : public AstExpr
	{
	public:
		ASTRTTI(AstExprConstantBool)

			AstExprConstantBool(const Location& location, bool value)
			: AstExpr(location)
			, value(value)
		{
		}

		virtual void visit(AstVisitor* visitor)
		{
			visitor->visit(this);
		}

		bool value;

		virtual ConstEvalResult constEval()
		{
			return static_cast<ConstEvalResult>(value);
		}
	};

	class AstExprConstantNumber : public AstExpr
	{
	public:
		ASTRTTI(AstExprConstantNumber)

			AstExprConstantNumber(const Location& location, double value)
			: AstExpr(location)
			, value(value)
		{
		}

		virtual void visit(AstVisitor* visitor)
		{
			visitor->visit(this);
		}

		double value;

		virtual ConstEvalResult constEval()
		{
			return ConstEvalResult::True;
		}
	};

	class AstExprConstantString : public AstExpr
	{
	public:
		ASTRTTI(AstExprConstantString)

			AstExprConstantString(const Location& location, const AstArray<char>& value)
			: AstExpr(location)
			, value(value)
		{
		}

		virtual void visit(AstVisitor* visitor)
		{
			visitor->visit(this);
		}

		AstArray<char> value;

		virtual ConstEvalResult constEval()
		{
			return ConstEvalResult::True;
		}
	};

	class AstExprLocal : public AstExpr
	{
	public:
		ASTRTTI(AstExprLocal)

			AstExprLocal(const Location& location, AstLocal* local, bool upvalue)
			: AstExpr(location)
			, local(local)
			, upvalue(upvalue)
		{
		}

		virtual void visit(AstVisitor* visitor)
		{
			visitor->visit(this);
		}

		AstLocal* local;
		bool upvalue;
	};

	class AstExprGlobal : public AstExpr
	{
	public:
		ASTRTTI(AstExprGlobal)

			AstExprGlobal(const Location& location, const AstName& name)
			: AstExpr(location)
			, name(name)
		{
		}

		virtual void visit(AstVisitor* visitor)
		{
			visitor->visit(this);
		}

		AstName name;
	};

	class AstExprVarargs : public AstExpr
	{
	public:
		ASTRTTI(AstExprVarargs)

			AstExprVarargs(const Location& location)
			: AstExpr(location)
		{
		}

		virtual void visit(AstVisitor* visitor)
		{
			visitor->visit(this);
		}
	};

	class AstExprCall : public AstExpr
	{
	public:
		ASTRTTI(AstExprCall)

			AstExprCall(const Location& location, AstExpr* func, const AstArray<AstExpr*>& args, bool self)
			: AstExpr(location)
			, func(func)
			, args(args)
			, self(self)
		{
		}

		virtual void visit(AstVisitor* visitor)
		{
			if (visitor->visit(this))
			{
				func->visit(visitor);

				for (size_t i = 0; i < args.size; ++i)
					args.data[i]->visit(visitor);
			}
		}

		AstExpr* func;
		AstArray<AstExpr*> args;
		bool self;
	};

	class AstExprIndexName : public AstExpr
	{
	public:
		ASTRTTI(AstExprIndexName)

			AstExprIndexName(const Location& location, AstExpr* expr, const AstName& index, const Location& indexLocation)
			: AstExpr(location)
			, expr(expr)
			, index(index)
			, indexLocation(indexLocation)
		{
		}

		virtual void visit(AstVisitor* visitor)
		{
			if (visitor->visit(this))
				expr->visit(visitor);
		}

		AstExpr* expr;
		AstName index;
		Location indexLocation;
	};

	class AstExprIndexExpr : public AstExpr
	{
	public:
		ASTRTTI(AstExprIndexExpr)

			AstExprIndexExpr(const Location& location, AstExpr* expr, AstExpr* index)
			: AstExpr(location)
			, expr(expr)
			, index(index)
		{
		}

		virtual void visit(AstVisitor* visitor)
		{
			if (visitor->visit(this))
			{
				expr->visit(visitor);
				index->visit(visitor);
			}
		}

		AstExpr* expr;
		AstExpr* index;
	};

	class AstExprFunction : public AstExpr
	{
	public:
		ASTRTTI(AstExprFunction)

			AstExprFunction(const Location& location, AstLocal* self, const AstArray<AstLocal*>& args, bool vararg, const AstArray<AstName*>& attributes, AstStat* body)
			: AstExpr(location)
			, self(self)
			, args(args)
			, vararg(vararg)
			, attributes(attributes)
			, body(body)
		{
		}

		virtual void visit(AstVisitor* visitor)
		{
			if (visitor->visit(this))
				body->visit(visitor);
		}

		AstLocal* self;
		AstArray<AstLocal*> args;
		bool vararg;
		AstArray<AstName*> attributes;

		AstStat* body;
	};

	class AstExprTable : public AstExpr
	{
	public:
		ASTRTTI(AstExprTable)

			AstExprTable(const Location& location, const AstArray<AstExpr*>& pairs)
			: AstExpr(location)
			, pairs(pairs)
		{
		}

		virtual void visit(AstVisitor* visitor)
		{
			if (visitor->visit(this))
			{
				for (size_t i = 0; i < pairs.size; ++i)
					if (pairs.data[i])
						pairs.data[i]->visit(visitor);
			}
		}

		AstArray<AstExpr*> pairs;
	};

	class AstExprUnary : public AstExpr
	{
	public:
		ASTRTTI(AstExprUnary)

			enum Op
		{
			Not,
			Minus,
			Len
		};

		AstExprUnary(const Location& location, Op op, AstExpr* expr)
			: AstExpr(location)
			, op(op)
			, expr(expr)
		{
		}

		virtual void visit(AstVisitor* visitor)
		{
			if (visitor->visit(this))
				expr->visit(visitor);
		}

		Op op;
		AstExpr* expr;
	};

	class AstExprBinary : public AstExpr
	{
	public:
		ASTRTTI(AstExprBinary)

			enum Op
		{
			Add,
			Sub,
			Mul,
			Div,
			Mod,
			Pow,
			Concat,
			CompareNe,
			CompareEq,
			CompareLt,
			CompareLe,
			CompareGt,
			CompareGe,
			And,
			Or
		};

		AstExprBinary(const Location& location, Op op, AstExpr* left, AstExpr* right)
			: AstExpr(location)
			, op(op)
			, left(left)
			, right(right)
		{
		}

		virtual void visit(AstVisitor* visitor)
		{
			if (visitor->visit(this))
			{
				left->visit(visitor);
				right->visit(visitor);
			}
		}

		Op op;
		AstExpr* left;
		AstExpr* right;
	};

	class AstStatBlock : public AstStat
	{
	public:
		ASTRTTI(AstStatBlock)

			AstStatBlock(const Location& location, const AstArray<AstStat*>& body)
			: AstStat(location)
			, body(body)
		{
		}

		virtual void visit(AstVisitor* visitor)
		{
			if (visitor->visit(this))
			{
				for (size_t i = 0; i < body.size; ++i)
					body.data[i]->visit(visitor);
			}
		}

		AstArray<AstStat*> body;
	};

	class AstStatIf : public AstStat
	{
	public:
		ASTRTTI(AstStatIf)

			AstStatIf(const Location& location, AstExpr* condition, AstStat* thenbody, AstStat* elsebody)
			: AstStat(location)
			, condition(condition)
			, thenbody(thenbody)
			, elsebody(elsebody)
		{
		}

		virtual void visit(AstVisitor* visitor)
		{
			if (visitor->visit(this))
			{
				condition->visit(visitor);
				thenbody->visit(visitor);

				if (elsebody)
					elsebody->visit(visitor);
			}
		}

		AstExpr* condition;
		AstStat* thenbody;
		AstStat* elsebody;
	};

	class AstStatWhile : public AstStat
	{
	public:
		ASTRTTI(AstStatWhile)

			AstStatWhile(const Location& location, AstExpr* condition, AstStat* body)
			: AstStat(location)
			, condition(condition)
			, body(body)
		{
		}

		virtual void visit(AstVisitor* visitor)
		{
			if (visitor->visit(this))
			{
				condition->visit(visitor);
				body->visit(visitor);
			}
		}

		AstExpr* condition;
		AstStat* body;
	};

	class AstStatRepeat : public AstStat
	{
	public:
		ASTRTTI(AstStatRepeat)

			AstStatRepeat(const Location& location, AstExpr* condition, AstStat* body)
			: AstStat(location)
			, condition(condition)
			, body(body)
		{
		}

		virtual void visit(AstVisitor* visitor)
		{
			if (visitor->visit(this))
			{
				condition->visit(visitor);
				body->visit(visitor);
			}
		}

		AstExpr* condition;
		AstStat* body;
	};

	class AstStatBreak : public AstStat
	{
	public:
		ASTRTTI(AstStatBreak)

			AstStatBreak(const Location& location)
			: AstStat(location)
		{
		}

		virtual void visit(AstVisitor* visitor)
		{
			visitor->visit(this);
		}
	};

	class AstStatReturn : public AstStat
	{
	public:
		ASTRTTI(AstStatReturn)

			AstStatReturn(const Location& location, const AstArray<AstExpr*>& list)
			: AstStat(location)
			, list(list)
		{
		}

		virtual void visit(AstVisitor* visitor)
		{
			if (visitor->visit(this))
			{
				for (size_t i = 0; i < list.size; ++i)
					list.data[i]->visit(visitor);
			}
		}

		AstArray<AstExpr*> list;
	};

	class AstStatExpr : public AstStat
	{
	public:
		ASTRTTI(AstStatExpr)

			AstStatExpr(const Location& location, AstExpr* expr)
			: AstStat(location)
			, expr(expr)
		{
		}

		virtual void visit(AstVisitor* visitor)
		{
			if (visitor->visit(this))
				expr->visit(visitor);
		}

		AstExpr* expr;
	};

	class AstStatLocal : public AstStat
	{
	public:
		ASTRTTI(AstStatLocal)

			AstStatLocal(const Location& location, const AstArray<AstLocal*>& vars, const AstArray<AstExpr*>& values)
			: AstStat(location)
			, vars(vars)
			, values(values)
		{
		}

		virtual void visit(AstVisitor* visitor)
		{
			if (visitor->visit(this))
			{
				for (size_t i = 0; i < values.size; ++i)
					values.data[i]->visit(visitor);
			}
		}

		AstArray<AstLocal*> vars;
		AstArray<AstExpr*> values;
	};

	class AstStatLocalFunction : public AstStat
	{
	public:
		ASTRTTI(AstStatLocalFunction)

			AstStatLocalFunction(const Location& location, AstLocal* var, AstExpr* body)
			: AstStat(location)
			, var(var)
			, body(body)
		{
		}

		virtual void visit(AstVisitor* visitor)
		{
			if (visitor->visit(this))
			{
				body->visit(visitor);
			}
		}

		AstLocal* var;
		AstExpr* body;
	};

	class AstStatFor : public AstStat
	{
	public:
		ASTRTTI(AstStatFor)

			AstStatFor(const Location& location, AstLocal* var, AstExpr* from, AstExpr* to, AstExpr* step, AstStat* body)
			: AstStat(location)
			, var(var)
			, from(from)
			, to(to)
			, step(step)
			, body(body)
		{
		}

		virtual void visit(AstVisitor* visitor)
		{
			if (visitor->visit(this))
			{
				from->visit(visitor);
				to->visit(visitor);

				if (step)
					step->visit(visitor);

				body->visit(visitor);
			}
		}

		AstLocal* var;
		AstExpr* from;
		AstExpr* to;
		AstExpr* step;
		AstStat* body;
	};

	class AstStatForIn : public AstStat
	{
	public:
		ASTRTTI(AstStatForIn)

			AstStatForIn(const Location& location, const AstArray<AstLocal*>& vars, const AstArray<AstExpr*>& values, AstStat* body)
			: AstStat(location)
			, vars(vars)
			, values(values)
			, body(body)
		{
		}

		AstArray<AstLocal*> vars;
		AstArray<AstExpr*> values;
		AstStat* body;

		virtual void visit(AstVisitor* visitor)
		{
			if (visitor->visit(this))
			{
				for (size_t i = 0; i < values.size; ++i)
					values.data[i]->visit(visitor);

				body->visit(visitor);
			}
		}
	};

	class AstStatAssign : public AstStat
	{
	public:
		ASTRTTI(AstStatAssign)

			AstStatAssign(const Location& location, const AstArray<AstExpr*>& vars, const AstArray<AstExpr*>& values)
			: AstStat(location)
			, vars(vars)
			, values(values)
		{
		}

		virtual void visit(AstVisitor* visitor)
		{
			if (visitor->visit(this))
			{
				for (size_t i = 0; i < vars.size; ++i)
					vars.data[i]->visit(visitor);

				for (size_t i = 0; i < values.size; ++i)
					values.data[i]->visit(visitor);
			}
		}

		AstArray<AstExpr*> vars;
		AstArray<AstExpr*> values;
	};

	class AstStatFunction : public AstStat
	{
	public:
		ASTRTTI(AstStatFunction)

			AstStatFunction(const Location& location, AstExpr* expr, AstExpr* body)
			: AstStat(location)
			, expr(expr)
			, body(body)
		{
		}

		virtual void visit(AstVisitor* visitor)
		{
			if (visitor->visit(this))
			{
				expr->visit(visitor);
				body->visit(visitor);
			}
		}

		AstExpr* expr;
		AstExpr* body;
	};

	inline const char* kReserved[] =
	{
		"and", "break", "do", "else", "elseif",
		"end", "false", "for", "function", "if",
		"in", "local", "nil", "not", "or", "repeat",
		"return", "then", "true", "until", "while"
	};

	struct Lexeme
	{
		enum Type
		{
			Eof = 0,

			// 1..255 means actual character values
			Char_END = 256,

			Equal,
			LessEqual,
			GreaterEqual,
			NotEqual,
			Dot2,
			Dot3,
			String,
			Number,
			Name,

			Reserved_BEGIN,
			ReservedAnd = Reserved_BEGIN,
			ReservedBreak,
			ReservedDo,
			ReservedElse,
			ReservedElseif,
			ReservedEnd,
			ReservedFalse,
			ReservedFor,
			ReservedFunction,
			ReservedIf,
			ReservedIn,
			ReservedLocal,
			ReservedNil,
			ReservedNot,
			ReservedOr,
			ReservedRepeat,
			ReservedReturn,
			ReservedThen,
			ReservedTrue,
			ReservedUntil,
			ReservedWhile,
			Reserved_END
		};

		Type type;
		Location location;

		union
		{
			const std::string* data; // String, Number
			const char* name; // Name
		};

		Lexeme(const Location& location, Type type)
			: type(type)
			, location(location)
		{
		}

		Lexeme(const Location& location, char character)
			: type(static_cast<Type>(static_cast<unsigned char>(character)))
			, location(location)
		{
		}

		Lexeme(const Location& location, Type type, const std::string* data)
			: type(type)
			, location(location)
			, data(data)
		{
			assert(type == String || type == Number);
		}

		Lexeme(const Location& location, Type type, const char* name)
			: type(type)
			, location(location)
			, name(name)
		{
			assert(type == Name);
		}

		std::string toString() const
		{
			switch (type)
			{
			case Eof:
				return "'<eof>'";

			case Equal:
				return "'=='";

			case LessEqual:
				return "'<='";

			case GreaterEqual:
				return "'>='";

			case NotEqual:
				return "'~='";

			case Dot2:
				return "'..'";

			case Dot3:
				return "'...'";

			case String:
				return TextFormat::format("\"%s\"", data->c_str());

			case Number:
				return TextFormat::format("'%s'", data->c_str());

			case Name:
				return TextFormat::format("'%s'", name);

			default:
				if (type < Char_END)
					return TextFormat::format("'%c'", type);
				if (type >= Reserved_BEGIN && type < Reserved_END)
					return TextFormat::format("'%s'", kReserved[type - Reserved_BEGIN]);
				return "'<unknown>'";
			}
		}
	};

	class AstNameTable
	{
		struct Entry
		{
			AstName value;
			Lexeme::Type type;
		};

		phmap::flat_hash_map<std::string, Entry> data;
		// std::map<const char*, Entr> data;

		Allocator& allocator;
	public:
		AstNameTable(Allocator& allocator)
			: allocator(allocator)
		{
		}

		AstName addStatic(const char* name, Lexeme::Type type = Lexeme::Name)
		{
			Entry entry = { AstName(name), type };

			assert(data.find(name) == data.end());
			data[name] = entry;

			return entry.value;
		}

		std::pair<AstName, Lexeme::Type> getOrAddWithType(const char* name)
		{
			auto it = data.find(name);
			if (it != data.end())
			{
				const Entry entry = it->second;
				return std::make_pair(entry.value, entry.type);
			}

			size_t nameLength = strlen(name);

			char* nameData = new (allocator) char[nameLength + 1];
			memcpy(nameData, name, nameLength + 1);

			Entry newEntry = { AstName(nameData), Lexeme::Name };
			data[nameData] = newEntry;

			return std::make_pair(newEntry.value, newEntry.type);
		}

		AstName getOrAdd(const char* name)
		{
			return getOrAddWithType(name).first;
		}
	};

	AstStat* parse(const char* buffer, size_t bufferSize, AstNameTable& names, Allocator& allocator);
}
