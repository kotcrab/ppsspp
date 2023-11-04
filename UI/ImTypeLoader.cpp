#include "ImTypeLoader.h"

#include "Common/Net/HTTPClient.h"
#include "Common/Data/Format/JSONReader.h"
#include "ImType.h"

static const char* host = "localhost";
static const int port = 18489;

ImTypeKind ResolveTypeKind(const std::string &input) {
	if (input == "ENUM") return ENUM;
	if (input == "TYPEDEF") return TYPEDEF;
	if (input == "POINTER") return POINTER;
	if (input == "ARRAY") return ARRAY;
	if (input == "STRUCTURE") return STRUCTURE;
	if (input == "UNION") return UNION;
	if (input == "FUNCTION_DEFINITION") return FUNCTION_DEFINITION;
	if (input == "BUILT_IN") return BUILT_IN;
	return UNKNOWN;
}

bool FetchRemoteTypes(std::vector<ImType>& types) {
	http::Client http;
	Buffer result;
	int code;

	if (!http.Resolve(host, port)) {
		return false;
	}
	bool cancelled = false;
	if (!http.Connect(1, 10.0, &cancelled)) {
		return false;
	}
	net::RequestProgress progress(&cancelled);
	code = http.GET(http::RequestParams("/v1/types"), &result, &progress);
	http.Disconnect();

	if (code != 200) {
		return false;
	}

	std::string json;
	result.TakeAll(&json);

	using namespace json;
	JsonReader reader(json.c_str(), json.size());
	if (!reader.ok()) {
		return false;
	}
	const JsonValue entries = reader.root().getArray("types")->value;
	if (entries.getTag() != JSON_ARRAY) {
		return false;
	}

	types.clear();
	for (const auto pEntry: entries) {
		JsonGet entry = pEntry->value;

		ImType type;
		type.name = entry.getString("name", "");
		type.displayName = entry.getString("displayName", "");
		type.pathName = entry.getString("pathName", "");
		type.length = entry.getInt("length", 0);
		type.alignedLength = entry.getInt("alignedLength", 0);
		type.zeroLength = entry.getBool("zeroLength", "");
		type.notYetDefined = entry.getBool("notYetDefined", "");
		type.description = entry.getString("description", "");

		auto kind = ResolveTypeKind(entry.getString("kind", ""));
		switch (kind) {
			case ENUM: {
				const JsonNode* enumEntries = entry.getArray("members");
				if (enumEntries) {
					for (const JsonNode* pEnumEntry: enumEntries->value) {
						JsonGet enumEntry = pEnumEntry->value;
						ImEnumMember member;
						member.name = enumEntry.getString("name", "");
						member.value = enumEntry.getInt("value", 0);
						member.comment = enumEntry.getString("comment", "");
						type.enumMembers.push_back(member);
					}
				}
				break;
			}
			case TYPEDEF:
				type.typeDefTypePathName = entry.getString("typePathName", "");
				type.typeDefBaseTypePathName = entry.getString("baseTypePathName", "");
				break;
			case POINTER:
				type.pointerTypePathName = entry.getString("typePathName", "");
				break;
			case ARRAY:
				type.arrayTypePathName = entry.getString("typePathName", "");
				type.arrayElementLength = entry.getInt("elementLength", 0);
				type.arrayElementCount = entry.getInt("elementCount", 0);
				break;
			case STRUCTURE:
			case UNION: {
				const JsonNode* compositeEntries = entry.getArray("members");
				if (compositeEntries) {
					for (const JsonNode* pCompositeEntry: compositeEntries->value) {
						JsonGet compositeEntry = pCompositeEntry->value;
						ImCompositeMember member;
						member.fieldName = compositeEntry.getString("fieldName", "");
						member.ordinal = compositeEntry.getInt("ordinal", 0);
						member.offset = compositeEntry.getInt("offset", 0);
						member.length = compositeEntry.getInt("length", 0);
						member.typePathName = compositeEntry.getString("typePathName", "");
						member.comment = compositeEntry.getString("comment", "");
						type.compositeMembers.push_back(member);
					}
				}
				break;
			}
			case FUNCTION_DEFINITION:
				type.functionPrototypeString = entry.getString("prototypeString", "");
				break;
			case BUILT_IN:
				type.builtInGroup = entry.getString("group", "");
				break;
			default:
				continue;
		}

		type.kind = kind;
		types.push_back(std::move(type));
	}

	return true;
}
