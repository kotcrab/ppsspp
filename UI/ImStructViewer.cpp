#include <iterator>
#include <regex>
#include <sstream>
#include "ImStructViewer.h"

#include "imgui.h"
#include "Core/Core.h"
#include "Core/MemMap.h"
#include "Core/MIPS/MIPSDebugInterface.h"
#include "ImTypeLoader.h"

static size_t GetTypeIndexByPathName(const std::vector<ImType>& types, const std::string& pathName) {
	auto it = std::find_if(types.begin(), types.end(), [&pathName](const ImType& x) { return x.pathName == pathName; });
	if (it == types.end()) {
		return -1;
	}
	return it - types.begin();
}

static void
ShowObject(const std::vector<ImType>& types, const u32 base, const u32 offset, const char* typePathName, const char* typeDisplayNameOverride,
		   const char* name) {
	size_t typeIndex = GetTypeIndexByPathName(types, typePathName);
	// Resolve typedefs as early as possible
	if (typeIndex != -1) {
		auto& type = types[typeIndex];
		if (type.kind == TYPEDEF) {
			ShowObject(types, base, offset, type.typeDefBaseTypePathName.c_str(), type.displayName.c_str(), name);
			return;
		}
	}

	ImGui::PushID((int) (base + offset));

	// Text and Tree nodes are less high than framed widgets, using AlignTextToFramePadding() we add vertical spacing to make the tree lines equal high.
	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(0);
	ImGui::AlignTextToFramePadding();
	ImGuiTreeNodeFlags leafFlags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_Bullet;

	if (strcmp(typePathName, "/void") == 0) {
		ImGui::TreeNodeEx("Field", leafFlags, "<void type>");
		ImGui::PopID();
		return;
	}
	if (typeIndex == -1) {
		ImGui::TreeNodeEx("Field", leafFlags, "<missing type: %s>", typePathName);
		ImGui::PopID();
		return;
	}
	if (!Memory::IsValidAddress(base)) {
		ImGui::TreeNodeEx("Field", leafFlags, "<invalid pointer>");
		ImGui::PopID();
		return;
	}

	auto& type = types[typeIndex];
	const char* effectiveDisplayName = typeDisplayNameOverride == nullptr ? type.displayName.c_str() : typeDisplayNameOverride;

	switch (type.kind) {
		case ENUM: {
			ImGui::TreeNodeEx("Field", leafFlags, "%s %s", effectiveDisplayName, name);
			ImGui::TableSetColumnIndex(1);
			ImGui::Text("+%X", offset);
			u64 value;
			switch (type.length) {
				case 1:
					value = Memory::Read_U8(base + offset);
					break;
				case 2:
					value = Memory::Read_U16(base + offset);
					break;
				case 4:
					value = Memory::Read_U32(base + offset);
					break;
				case 8:
					value = Memory::Read_U64(base + offset);
					break;
			}
			std::stringstream ss;
			bool hasPrevious = false;
			for (const auto& member: type.enumMembers) {
				if (value & member.value) {
					if (hasPrevious) {
						ss << " | ";
					}
					ss << member.name;
					hasPrevious = true;
				}
			}
			std::string s = ss.str();
			ImGui::TableSetColumnIndex(2);
			ImGui::Text("%llX (%s)", value, s.c_str());
			break;
		}
		case POINTER: {
			u32 pointer = Memory::Read_U32(base + offset);
			bool nodeOpen = ImGui::TreeNodeEx("Pointer", 0, "%s %s", effectiveDisplayName, name);
			ImGui::TableSetColumnIndex(1);
			ImGui::Text("+%X", offset);
			ImGui::TableSetColumnIndex(2);
			ImGui::Text("%X", pointer);
			if (pointer != 0) {
				ImGui::SameLine();
				ImGui::Button("Show memory"); // TODO
			}
			if (nodeOpen) {
				size_t pointedTypeIndex = GetTypeIndexByPathName(types, type.pointerTypePathName);
				if (pointedTypeIndex != -1) {
					auto& pointedType = types[pointedTypeIndex];

					auto countStateId = ImGui::GetID("PointerElementCount");
					int pointerElementCount = ImGui::GetStateStorage()->GetInt(countStateId, 1);

					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					if (ImGui::Button("Show more")) {
						ImGui::GetStateStorage()->SetInt(countStateId, pointerElementCount + 1);
					}
					if (pointerElementCount > 1) {
						ImGui::SameLine();
						ImGui::Text("(showing %X)", pointerElementCount);
					}

					for (int i = 0; i < pointerElementCount; i++) {
						ShowObject(types, pointer, i * pointedType.length, type.pointerTypePathName.c_str(), nullptr, "");
					}
				}
				ImGui::TreePop();
			}
			break;
		}
		case ARRAY: {
			bool nodeOpen = ImGui::TreeNodeEx("Array", 0, "%s %s", effectiveDisplayName, name);
			ImGui::TableSetColumnIndex(1);
			ImGui::Text("+%X", offset);

			size_t arrayTypeIndex = GetTypeIndexByPathName(types, type.arrayTypePathName);
			auto& arrayType = types[arrayTypeIndex];
			bool charType;
			if (arrayType.kind == TYPEDEF) {
				arrayTypeIndex = GetTypeIndexByPathName(types, arrayType.typeDefBaseTypePathName);
				auto& baseArrayType = types[arrayTypeIndex];
				charType = baseArrayType.pathName == "/char";
			} else {
				charType = arrayType.pathName == "/char";
			}

			if (charType && type.arrayElementLength == 1) {
				ImGui::TableSetColumnIndex(2);
				const char* strPtr = Memory::GetCharPointerUnchecked(base + offset);
				std::string s(strPtr, strPtr + type.arrayElementCount);
				s = std::regex_replace(s, std::regex("\n"), "\\n");
				ImGui::Text("\"%s\"", s.c_str());
			}

			if (nodeOpen) {
				for (int i = 0; i < type.arrayElementCount; i++) {
					ShowObject(types, base + offset, i * type.arrayElementLength, type.arrayTypePathName.c_str(), nullptr, "");
				}
				ImGui::TreePop();
			}
			break;
		}
		case STRUCTURE:
		case UNION: {
			bool nodeOpen = ImGui::TreeNodeEx("Object", 0, "%s %s", effectiveDisplayName, name);
			ImGui::TableSetColumnIndex(1);
			ImGui::Text("+%X", offset);
			ImGui::TableSetColumnIndex(2);
			ImGui::Text("%X", base + offset);
			ImGui::SameLine();
			ImGui::Button("Show memory"); // TODO
			if (nodeOpen) {
				for (const auto& member: type.compositeMembers) {
					ShowObject(types, base + offset, member.offset, member.typePathName.c_str(), nullptr, member.fieldName.c_str());
				}
				ImGui::TreePop();
			}
			break;
		}
		case FUNCTION_DEFINITION:
			ImGui::TreeNodeEx("Field", leafFlags, "%s %s", effectiveDisplayName, name);
			ImGui::TableSetColumnIndex(1);
			ImGui::Text("+%X", offset);
			ImGui::TableSetColumnIndex(2);
			ImGui::Text("<function definition>");
			break;
		case BUILT_IN: {
			// TODO string built ins
			bool boolType = type.pathName == "/bool";
			bool floatType = type.pathName == "/float";
			bool charType = type.pathName == "/char" || type.pathName == "/uchar";
			bool longlongType = type.pathName == "/longlong" || type.pathName == "/ulonglong" || type.pathName == "/undefined8";
			bool intType = type.pathName == "/int" || type.pathName == "/uint" || type.pathName == "/long" || type.pathName == "/ulong" ||
						   type.pathName == "/dword" || type.pathName == "/undefined4";
			bool shortType = type.pathName == "/short" || type.pathName == "/ushort" || type.pathName == "/undefined2";
			bool byteType = type.pathName == "/byte" || type.pathName == "/undefined1";
			if (boolType || floatType || charType || longlongType || intType || shortType || byteType) {
				ImGui::TreeNodeEx("Field", leafFlags, "%s %s", effectiveDisplayName, name);
				ImGui::TableSetColumnIndex(2);
				if (boolType) {
					ImGui::Text("%s", Memory::Read_U8(base + offset) ? "true" : "false");
				} else if (floatType) {
					ImGui::Text("%f", Memory::Read_Float(base + offset));
				} else if (longlongType) {
					ImGui::Text("%llx", Memory::Read_U64(base + offset));
				} else if (intType) {
					ImGui::Text("%X", Memory::Read_U32(base + offset));
				} else if (shortType) {
					ImGui::Text("%X", Memory::Read_U16(base + offset));
				} else {
					ImGui::Text("%X", Memory::Read_U8(base + offset));
				}
			} else {
				ImGui::TreeNodeEx("Field", leafFlags, "<unmapped built in: %s> %s", typePathName, name);
			}
			ImGui::TableSetColumnIndex(1);
			ImGui::Text("+%X", offset);
			break;
		}
		default:
			ImGui::TreeNodeEx("Field", leafFlags, "<unknown type: %s> %s", typePathName, name);
			ImGui::TableSetColumnIndex(1);
			ImGui::Text("+%X", offset);
			break;
	}

	ImGui::PopID();
}

static void ShowStructViewer(const std::vector<ImType>& types, bool* p_open) {
	ImGui::SetNextWindowSize(ImVec2(430, 450), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Struct viewer", p_open)) {
		ImGui::End();
		return;
	}

	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, 2));
	if (ImGui::BeginTable("##split", 3,
						  ImGuiTableFlags_BordersOuter | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Struct");
		ImGui::TableSetupColumn("Offset");
		ImGui::TableSetupColumn("Contents");
		ImGui::TableHeadersRow();

		ShowObject(types, 0x09007890, 0, "/CCC/DatParser", nullptr, "mainMenuDatParser");
		ShowObject(types, 0x08a2e694, 0, "/CCC/heap/HeapCtl", nullptr, "heapGData");
		ShowObject(types, 0x08b67270, 0, "/CCC/heap/HeapCtl", nullptr, "heapEDRAM");
		ShowObject(types, 0x08a28b80, 0, "/CCC/game/CLogo *", nullptr, "cLogo");
		ImGui::EndTable();
	}
	ImGui::PopStyleVar();
	ImGui::End();
}

void ImStructViewer::Draw() {
	if (!typesFetched) {
		typesFetched = true;
		FetchRemoteTypes(types);
	}

	static bool show_demo_window = true;
	if (show_demo_window)
		ImGui::ShowDemoWindow(&show_demo_window);
	if (currentDebugMIPS->isAlive() && Memory::IsActive()) {
		u32 memBase = PSP_GetUserMemoryBase();
		void* memData = (void*) Memory::GetPointerUnchecked(memBase);
		editor.DrawWindow("Memory 1", memData, 0x1800000, memBase);
		editor.DrawWindow("Memory 2", memData, 0x1800000, memBase);
		editor.DrawWindow("Memory 3", memData, 0x1800000, memBase);
		editor.DrawWindow("Memory 4", memData, 0x1800000, memBase);

		static bool open = true;
		ShowStructViewer(types, &open);
	}
}
