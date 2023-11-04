#pragma once

#include <string>
#include <vector>

struct ImEnumMember {
	std::string name;
	long value = 0;
	std::string comment;
};

struct ImCompositeMember {
	std::string fieldName;
	int ordinal = 0;
	int offset = 0;
	int length = 0;
	std::string typePathName;
	std::string comment;
};

enum ImTypeKind {
	ENUM,
	TYPEDEF,
	POINTER,
	ARRAY,
	STRUCTURE,
	UNION,
	FUNCTION_DEFINITION,
	BUILT_IN,
	UNKNOWN,
};

struct ImType {
	ImTypeKind kind;
	std::string name;
	std::string displayName;
	std::string pathName;
	int length = 0;
	int alignedLength = 0;
	bool zeroLength = false;
	bool notYetDefined = false;
	std::string description;

	std::vector<ImCompositeMember> compositeMembers;
	std::vector<ImEnumMember> enumMembers;
	std::string pointerTypePathName;
	std::string typeDefTypePathName;
	std::string typeDefBaseTypePathName;
	std::string arrayTypePathName;
	int arrayElementLength = 0;
	int arrayElementCount = 0;
	std::string functionPrototypeString;
	std::string builtInGroup;
};
