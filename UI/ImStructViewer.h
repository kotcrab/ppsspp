#pragma once

#include "ImMemoryView.hpp"
#include "ImType.h"

#include <vector>

class ImStructViewer {
public:
	void Draw();

private:
	bool typesFetched = false;
	std::vector<ImType> types;
	MemoryEditor editor;
};

