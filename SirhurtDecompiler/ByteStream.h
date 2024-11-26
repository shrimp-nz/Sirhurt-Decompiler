#pragma once
#include <vector>
#include <string>

using byte = uint8_t;

class ByteStream : std::vector<byte>
{
	typedef std::vector<byte> vector;

	template<size_type sz>
	void resize_for_write()
	{
		if (capacity() - size() < sz)
			reserve(capacity() + sz * 2);
	}

	void resize_for_write(size_type sz)
	{
		if (capacity() - size() < sz)
			reserve(capacity() + sz * 2);
	}
public:
	void clear()
	{
		vector::clear();
	}

	// void insert()

	template<typename T>
	ByteStream& operator<<(const T v)
	{
		resize_for_write<sizeof(T)>();

		const auto pointer = (byte*)&v;
		for (unsigned int i = 0; i < sizeof(T); i++)
		{
			push_back(pointer[i]);
		}
		return *this;
	}

	ByteStream& operator<<(const char* v)
	{
		insert(end(), v, v + strlen(v));
		return *this;
	}

	ByteStream& operator<<(const std::string& v)
	{
		insert(end(), v.cbegin(), v.cend());
		return *this;
	}

	template<typename _Elem, typename _Traits, typename _Alloc>
	ByteStream& operator<<(std::basic_string<_Elem, _Traits, _Alloc> & v)
	{
		return operator<<(v.c_str());
	}

	template<typename _Elem, typename _Traits, typename _Alloc>
	ByteStream& operator<<(std::basic_string<_Elem, _Traits, _Alloc> v)
	{
		return operator<<(v.c_str());
	}

	ByteStream& operator<<(const int v)
	{
		resize_for_write<sizeof(int)>();

		/*
		auto i = (uint)v;
		do
		{
			push_back((i & 0x7F) | 0x80);
			i = i >> 7;
		} while (i != 0);

		at(size() - 1) &= 0x7F;*/

		unsigned int i = v;
		do
		{
			push_back((i & 0x7F) | (i > 0x7F) << 7);
			i >>= 7;
		} while (i);

		return *this;
	}

	std::vector<byte>& vec()
	{
		return *this;
	}
};