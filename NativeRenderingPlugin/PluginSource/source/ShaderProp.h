#pragma once
#include "PlatformBase.h"
#include <assert.h>

enum PropType {
	Float,
	Vector2,
	Vector3,
	Vector4,
	Matrix,
	FloatBlock
};

const std::string propTypeStrings[] = {
	"Float",
	"Vector2",
	"Vector3",
	"Vector4",
	"Matrix",
};


struct ShaderProp {
	ShaderProp(PropType type_, std::string name_)
		: type(type_)
		, name(name_)
	{
#if SUPPORT_OPENGL_UNIFIED || SUPPORT_OPENGL_LEGACY
		uniformIndex = UNIFORM_UNSET;
#endif
		offset = 0;
		size = 0;
		arraySize = 0;
	}

	PropType type;
	const std::string typeString() { return propTypeStrings[(size_t)type]; }
	std::string name;

	/*
	float value(int n) {
		if (!constantBuffer || size == 0) return 0.0f;
		return *((float*)(constantBuffer + offset + n * sizeof(float)));
	}
	*/

#if SUPPORT_OPENGL_UNIFIED || SUPPORT_OPENGL_LEGACY
	static const int UNIFORM_UNSET = -2;
	static const int UNIFORM_INVALID = -1;
	int uniformIndex;
#endif

	uint16_t offset;
	uint16_t size;
	uint16_t arraySize;

	static PropType typeForSize(uint16_t size) {
		switch (size) {
		case sizeof(float) : return Float;
		case 2 * sizeof(float) : return Vector2;
		case 3 * sizeof(float) : return Vector3;
		case 4 * sizeof(float) : return Vector4;
		case 16 * sizeof(float) : return Matrix;
		default: return FloatBlock;
		}
	}

	static uint16_t sizeForType(PropType type) {
		switch (type) {
		case Float: return sizeof(float);
		case Vector2: return 2 * sizeof(float);
		case Vector3: return 3 * sizeof(float);
		case Vector4: return 4 * sizeof(float);
		case Matrix: return 16 * sizeof(float);
		default: assert(false); return 0;
		}
	}
};

